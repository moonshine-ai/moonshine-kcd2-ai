#include "doctest.h"

#include "gamelog.h"
#include "memory.h"

using namespace moonshine;

namespace {

constexpr const char* kSoul = "kkut_additive_man_47";

// A dump as Moonshine.Dump prints it, for a soul met twice before: the first talk two days
// ago in the evening, the second yesterday morning, and the conversation now opening.
const char* kDump =
    "<12:04:30> BRIDGE state npc=kkut_additive_man_47 met=3 first=1000000 last=1250000 rev=0 "
    "entries=8 quest=c00183e1da7e accepted=true handed=false done=false stat:trust=0.750\n"
    "<12:04:30> BRIDGE known npc=kkut_additive_man_47 kkut_additive_man_47 khor_woman_2\n"
    "<12:04:30> BRIDGE known npc=kkut_additive_man_47 name:humphrey\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 1/8 k=gap t=1000000 s=rch=0.5 hour=18.5 rain=0\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 2/8 k=p t=nil s=Which way to the mill?\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 3/8 k=n t=nil s=Past the smithy, then left.\\nMind the dog.\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 4/8 k=gap t=1150000 s=rch=0.5 hour=7 rain=0.8\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 5/8 k=p t=nil s=I said BR\\IDGE left and you believed me\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 6/8 k=n t=nil s=Get on with you.\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 7/8 k=p t=nil s=Ta.\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 8/8 k=gap t=1250000 s=rch=0.5 hour=12 rain=0\n"
    "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 end\n";

SoulRecord dumped() { return record_from_lines(kDump); }

}  // namespace

TEST_CASE("the header of a soul's record is read field by field, not by searching") {
    const std::optional<SoulRecord> header = read_state_line(
        "<12:04:30> BRIDGE state npc=kkut_additive_man_47 met=3 first=1000000 last=1250000 rev=2 "
        "entries=8 quest=c00183e1da7e accepted=true handed=false done=true stat:trust=0.750");
    REQUIRE(header.has_value());
    CHECK(header->soul == kSoul);
    CHECK(header->met == 3);
    // "met=" contains "t=" and "first=" contains "t=": a search would read the wrong number.
    CHECK(header->first == doctest::Approx(1000000));
    CHECK(header->last == doctest::Approx(1250000));
    CHECK(header->rev == 2);
    CHECK(header->entries == 8);
    CHECK(header->quest == "c00183e1da7e");
    CHECK(header->facts.accepted);
    CHECK_FALSE(header->facts.handed);
    CHECK(header->facts.done);
    REQUIRE(header->stats.size() == 1);
    CHECK(header->stats[0].first == "trust");
    CHECK(header->stats[0].second == doctest::Approx(0.75));

    // A first meeting has no quest and no stats, and a clock the mod could not read is nil.
    const std::optional<SoulRecord> fresh =
        read_state_line("BRIDGE state npc=kkut_nameless_9 met=1 first=nil last=nil rev=0 entries=1");
    REQUIRE(fresh.has_value());
    CHECK(fresh->quest.empty());
    CHECK(fresh->first == kUnread);
    CHECK_FALSE(fresh->facts.accepted);

    // The mod's other state chatter is not a header.
    CHECK_FALSE(read_state_line("BRIDGE state saved via OnSave version=1 bytes=120").has_value());
    CHECK_FALSE(read_state_line("BRIDGE state quest c00183e1da7e accept accepted=true").has_value());
    CHECK_FALSE(read_state_line("BRIDGE state compacted npc=kkut_additive_man_47 n=7 rev=1").has_value());
    CHECK_FALSE(read_state_line("BRIDGE state compact refused npc=kkut_additive_man_47 rev=0").has_value());
}

TEST_CASE("a memory line carries its kind, its clock and its text, unescaped") {
    const std::optional<MemoryLine> line = read_memory_line(
        "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 3/8 k=n t=nil "
        "s=Past the smithy, then left.\\nMind the dog. k=fake t=9 s=not a field");
    REQUIRE(line.has_value());
    CHECK(line->soul == kSoul);
    CHECK(line->index == 3);
    CHECK(line->total == 8);
    CHECK_FALSE(line->end);
    CHECK(line->entry.kind == "n");
    CHECK(line->entry.t == kUnread);
    // Everything after s= is the text, key-shaped words included; the escaped newline
    // comes back as one.
    CHECK(line->entry.text == "Past the smithy, then left.\nMind the dog. k=fake t=9 s=not a field");

    const std::optional<MemoryLine> gap = read_memory_line(
        "BRIDGE memory npc=kkut_additive_man_47 1/8 k=gap t=1000000 s=rch=0.5 hour=18.5 rain=0");
    REQUIRE(gap.has_value());
    CHECK(gap->entry.kind == "gap");
    CHECK(gap->entry.t == doctest::Approx(1000000));
    CHECK(gap->entry.text == "rch=0.5 hour=18.5 rain=0");

    const std::optional<MemoryLine> end = read_memory_line("BRIDGE memory npc=kkut_additive_man_47 end");
    REQUIRE(end.has_value());
    CHECK(end->end);
    CHECK(end->soul == kSoul);

    CHECK_FALSE(read_memory_line("BRIDGE memory line 3000 chars").has_value());
}

TEST_CASE("the mod's escaping is undone, and a remembered marker cannot pass for one") {
    CHECK(unescape_memory("a\\\\b") == "a\\b");
    CHECK(unescape_memory("one\\ntwo\\rthree") == "one\ntwo\rthree");
    CHECK(unescape_memory("BR\\IDGE left") == "BRIDGE left");
    // A lone backslash before anything else is just a backslash.
    CHECK(unescape_memory("C:\\temp") == "C:\\temp");

    // Through the classifier: the player once said "BRIDGE left", and the memory of it is a
    // Memory event, not a Left one -- and its text has the word whole again.
    const Event event = classify(
        "<12:04:30> BRIDGE memory npc=kkut_additive_man_47 5/8 k=p t=nil s=I said BR\\IDGE left");
    CHECK(event.marker == Marker::Memory);
    CHECK(event.memory.entry.text == "I said BRIDGE left");
    // And a memory of a typed line is not the player typing it again.
    CHECK(classify("BRIDGE memory npc=x 1/1 k=p t=nil s=BR\\IDGE said hello").marker ==
          Marker::Memory);
}

TEST_CASE("the classifier tells the record's lines and the store's news apart") {
    const Event header = classify(kDump);
    CHECK(header.marker == Marker::State);
    CHECK(header.soul == kSoul);
    CHECK(header.state.met == 3);

    const Event loaded = classify("<12:04:00> BRIDGE state loaded via OnLoad version=1 souls=3");
    CHECK(loaded.marker == Marker::WorldChanged);
    CHECK(loaded.said == "loaded via OnLoad version=1 souls=3");
    CHECK(classify("BRIDGE state reset, a world is arriving").marker == Marker::WorldChanged);
    CHECK(classify("BRIDGE state saved via OnSave version=1 bytes=4").marker == Marker::None);
    CHECK(classify("BRIDGE state quest c00183e1da7e accept accepted=true").marker == Marker::None);
}

TEST_CASE("a whole dump read from text is the record the header promised") {
    const SoulRecord record = dumped();
    CHECK(record.soul == kSoul);
    CHECK(record.met == 3);
    CHECK(record.entries == 8);
    CHECK(record.memory.size() == 8);
    CHECK(record.complete);
    CHECK(record.memory[2].text == "Past the smithy, then left.\nMind the dog.");
    CHECK(record.memory[4].text == "I said BRIDGE left and you believed me");
    CHECK(record_from_lines("nothing here\n").empty());
    // Who he knows of, off as many known lines as the mod needed.
    REQUIRE(record.known.size() == 3);
    CHECK(record.knows("khor_woman_2"));
    CHECK(record.knows("name:humphrey"));
    CHECK_FALSE(record.knows("khor_woman_3"));
}

TEST_CASE("a known line names its soul and its tokens, and is its own marker") {
    const std::optional<KnownLine> line =
        read_known_line("<12:04:30> BRIDGE known npc=kkut_additive_man_47 khor_woman_2 name:humphrey\r");
    REQUIRE(line);
    CHECK(line->soul == kSoul);
    REQUIRE(line->tokens.size() == 2);
    CHECK(line->tokens[1] == "name:humphrey");
    CHECK_FALSE(read_known_line("<12:04:30> BRIDGE memory npc=kkut_additive_man_47 end"));
    const Event event = classify("<12:04:30> BRIDGE known npc=kkut_additive_man_47 khor_woman_2");
    CHECK(event.marker == Marker::Known);
    CHECK(event.soul == kSoul);
    CHECK(event.known.tokens == std::vector<std::string>{"khor_woman_2"});
}

TEST_CASE("a gap is said in the words a person would use, never to the second") {
    CHECK(ago_words(kUnread).empty());
    CHECK(ago_words(600) == "less than an hour ago");
    CHECK(ago_words(3 * 3600) == "a few hours ago");
    CHECK(ago_words(10 * 3600) == "earlier today");
    CHECK(ago_words(30 * 3600) == "yesterday");
    CHECK(ago_words(2 * 86400) == "two days ago");
    CHECK(ago_words(5 * 86400 + 3000) == "five days ago");
    CHECK(ago_words(8 * 86400) == "a week ago");
    CHECK(ago_words(15 * 86400) == "two weeks ago");
    CHECK(ago_words(35 * 86400) == "a month ago");
    CHECK(ago_words(95 * 86400) == "three months ago");
    CHECK(ago_words(400 * 86400) == "a year ago");
}

TEST_CASE("the memory block tells the earlier conversations, dated from now, in speech") {
    const std::string block = memory_block(dumped(), kPlayer);
    // How often, from the header, not counting the meeting that is opening.
    CHECK(block.find("You have spoken with Henry twice before.") != std::string::npos);
    // Each earlier conversation is headed by when it was, against the clock of the one now
    // opening, and what the light was like from its hour.
    CHECK(block.find("Three days ago, in the evening, Henry came to you:") != std::string::npos);
    CHECK(block.find("Yesterday, in the morning, Henry came to you:") != std::string::npos);
    // The lines, as speech, with the soul's own marked as theirs and flattened to one line.
    CHECK(block.find("Henry: Which way to the mill?") != std::string::npos);
    CHECK(block.find("You: Past the smithy, then left. Mind the dog.") != std::string::npos);
    CHECK(block.find("You: Get on with you.") != std::string::npos);
    // The conversation opening now is not a memory.
    CHECK(block.find("hour=12") == std::string::npos);
    // In order: the older talk before the newer.
    CHECK(block.find("Three days ago") < block.find("Yesterday"));
}

TEST_CASE("a first meeting and an empty record have nothing to remember") {
    CHECK(memory_block(SoulRecord{}, kPlayer).empty());
    const SoulRecord first = record_from_lines(
        "BRIDGE state npc=kkut_nameless_9 met=1 first=5 last=5 rev=0 entries=1\n"
        "BRIDGE memory npc=kkut_nameless_9 1/1 k=gap t=5 s=hour=9\n"
        "BRIDGE memory npc=kkut_nameless_9 end\n");
    CHECK(memory_block(first, kPlayer).empty());
}

TEST_CASE("a summary stands in the block as written, and says when they last spoke") {
    const SoulRecord record = record_from_lines(
        "BRIDGE state npc=kkut_nameless_9 met=4 first=0 last=400000 rev=1 entries=2\n"
        "BRIDGE memory npc=kkut_nameless_9 1/2 k=sum t=0 s=Henry came to you twice about the mill and you sent him past the smithy.\n"
        "BRIDGE memory npc=kkut_nameless_9 2/2 k=gap t=400000 s=hour=9\n"
        "BRIDGE memory npc=kkut_nameless_9 end\n");
    const std::string block = memory_block(record, kPlayer);
    CHECK(block.find("You have spoken with Henry three times before.") != std::string::npos);
    CHECK(block.find("Henry came to you twice about the mill") != std::string::npos);
    // A summary's clock is the oldest thing in it, so the block says outright when the last
    // talk was -- which, with every gap folded away, it cannot: nothing is said rather than
    // something wrong.
    CHECK(block.find("You last spoke") == std::string::npos);

    // With a gap left after the summary, it can.
    const SoulRecord dated = record_from_lines(
        "BRIDGE state npc=kkut_nameless_9 met=4 first=0 last=400000 rev=1 entries=3\n"
        "BRIDGE memory npc=kkut_nameless_9 1/3 k=gap t=300000 s=hour=9\n"
        "BRIDGE memory npc=kkut_nameless_9 2/3 k=sum t=0 s=You sent him to the mill.\n"
        "BRIDGE memory npc=kkut_nameless_9 3/3 k=gap t=400000 s=hour=9\n"
        "BRIDGE memory npc=kkut_nameless_9 end\n");
    CHECK(memory_block(dated, kPlayer).find("You last spoke yesterday.") != std::string::npos);
}

TEST_CASE("older conversations are dropped whole once the block is full, newest kept entire") {
    // Three earlier talks of 400 words each: the newest is always shown, the one before it
    // fits under the budget with it only if the budget allows, and the oldest never does.
    std::string text = "BRIDGE state npc=x met=4 first=0 last=900000 rev=0 entries=0\n";
    for (int talk = 0; talk < 3; ++talk) {
        text += "BRIDGE memory npc=x 0/0 k=gap t=" + std::to_string(100000 * (talk + 1)) + " s=hour=9\n";
        std::string words;
        for (int w = 0; w < 400; ++w) words += "word" + std::to_string(talk) + " ";
        text += "BRIDGE memory npc=x 0/0 k=p t=nil s=" + words + "\n";
    }
    text += "BRIDGE memory npc=x 0/0 k=gap t=900000 s=hour=9\n";
    const std::string block = memory_block(record_from_lines(text), kPlayer);
    CHECK(block.find("word2") != std::string::npos);
    // 400 + 400 is over kMemoryRawWords, so the middle talk is out along with the oldest.
    CHECK(block.find("word1") == std::string::npos);
    CHECK(block.find("word0") == std::string::npos);
}

TEST_CASE("compaction is due when the earlier lines outrun the budget, and asks for a paragraph") {
    // Under budget: nothing to do.
    CHECK_FALSE(compaction_for(dumped(), "Bartel", kPlayer).has_value());

    std::string text = "BRIDGE state npc=x met=3 first=0 last=900000 rev=2 entries=0\n";
    text += "BRIDGE memory npc=x 0/0 k=sum t=0 s=Henry asked you the way once.\n";
    text += "BRIDGE memory npc=x 0/0 k=gap t=100000 s=hour=9\n";
    std::string words;
    for (int w = 0; w < 700; ++w) words += "blah ";
    text += "BRIDGE memory npc=x 0/0 k=p t=nil s=" + words + "\n";
    text += "BRIDGE memory npc=x 0/0 k=n t=nil s=Enough.\n";
    text += "BRIDGE memory npc=x 0/0 k=gap t=300000 s=hour=20\n";
    text += "BRIDGE memory npc=x 0/0 k=p t=nil s=Still here?\n";
    text += "BRIDGE memory npc=x 0/0 k=gap t=900000 s=hour=9\n";
    const SoulRecord record = record_from_lines(text);
    const std::optional<Compaction> due = compaction_for(record, "Bartel", kPlayer);
    REQUIRE(due.has_value());
    // Everything before the conversation now opening: the summary and both earlier talks,
    // six entries.
    CHECK(due->n == 6);
    REQUIRE(due->messages.size() == 2);
    CHECK(due->messages[0].role == "system");
    CHECK(due->messages[0].content.find("Bartel") != std::string::npos);
    CHECK(due->messages[0].content.find("second person") != std::string::npos);
    const std::string& account = due->messages[1].content;
    // The earlier summary is folded in too, and each talk is placed against the one before
    // it rather than against today, since the paragraph will be read for weeks.
    CHECK(account.find("What you already remembered:\nHenry asked you the way once.") != std::string::npos);
    CHECK(account.find("Two days later, in the evening, Henry came to you:") != std::string::npos);
    CHECK(account.find("You: Enough.") != std::string::npos);
    CHECK(account.find("Henry: Still here?") != std::string::npos);
    // The directive names the revision the record was at, so the mod can refuse a stale one.
    CHECK(compact_directive(record.soul, record.rev, due->n, "He asked. You told him.") ==
          "@memory x compact 2 6 23:He asked. You told him.");
}

TEST_CASE("a paragraph the model wrote is refused when it is not a paragraph") {
    CHECK(summary_trouble("Henry came to you about the mill and you sent him on.").empty());
    CHECK_FALSE(summary_trouble("").empty());
    CHECK_FALSE(summary_trouble("   \n ").empty());
    CHECK_FALSE(summary_trouble("Henry: where is it? You: over there.").empty());
    CHECK_FALSE(summary_trouble("As an AI language model I cannot remember.").empty());
    std::string long_text;
    for (int i = 0; i < kSummaryMostWords; ++i) long_text += "word ";
    CHECK_FALSE(summary_trouble(long_text).empty());
}

TEST_CASE("the directives count bytes of the flattened text, as the mod will read them") {
    // Two lines and a run of spaces become one line with one space, and the count is of that.
    CHECK(said_directive("  Which way\n\nto  Rattay? ") == "@said 20:Which way to Rattay?");
    CHECK(compact_directive("kkut_x", 0, 3, "One.\nTwo.") == "@memory kkut_x compact 0 3 9:One. Two.");
    // Bytes, not characters: the mod's string.sub counts bytes.
    CHECK(said_directive("Žák") == "@said 5:Žák");
    CHECK(words_in("  one two\nthree ") == 3);
    CHECK(words_in("") == 0);
}
