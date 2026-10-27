#include "memory.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace moonshine {

namespace {

constexpr std::string_view kStateHeader = "BRIDGE state npc=";
constexpr std::string_view kMemoryHeader = "BRIDGE memory npc=";
constexpr std::string_view kKnownHeader = "BRIDGE known npc=";

std::string_view trim(std::string_view s) {
    auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// The mod writes Lua's "nil" for a clock it could not read, and %d for one it could.
double clock_of(std::string_view text) {
    if (text.empty() || text == "nil") return kUnread;
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() ? kUnread : value;
}

int count_of(std::string_view text) {
    int value = 0;
    for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) break;
        value = value * 10 + (c - '0');
    }
    return value;
}

// The `key=value` fields of a line, split at spaces, up to and not including a key the
// caller names as running to the end of the line. Split rather than searched for, because
// "met=" contains "t=" and a search for the clock would find the count.
struct Fields {
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    std::string_view rest;  // the value of the terminal key, or empty

    std::string_view get(std::string_view key) const {
        for (const auto& [k, v] : pairs) {
            if (k == key) return v;
        }
        return {};
    }
    bool has(std::string_view key) const {
        for (const auto& [k, v] : pairs) {
            if (k == key) return true;
        }
        return false;
    }
};

Fields split_fields(std::string_view text, std::string_view terminal) {
    Fields fields;
    while (!text.empty()) {
        const std::size_t start = text.find_first_not_of(' ');
        if (start == std::string_view::npos) break;
        text.remove_prefix(start);
        if (!terminal.empty() && text.substr(0, terminal.size()) == terminal) {
            fields.rest = text.substr(terminal.size());
            break;
        }
        const std::size_t space = text.find(' ');
        const std::string_view token = text.substr(0, space);
        const std::size_t eq = token.find('=');
        if (eq == std::string_view::npos) {
            fields.pairs.emplace_back(token, std::string_view{});
        } else {
            fields.pairs.emplace_back(token.substr(0, eq), token.substr(eq + 1));
        }
        if (space == std::string_view::npos) break;
        text.remove_prefix(space);
    }
    return fields;
}

// The hour off a gap entry's readings, which are the begin line's fields verbatim.
double hour_of(std::string_view readings) {
    return clock_of(split_fields(readings, {}).get("hour"));
}

std::string_view number_words(int n) {
    static constexpr std::string_view kWords[] = {"no",    "one",   "two",    "three", "four",
                                                  "five",  "six",   "seven",  "eight", "nine",
                                                  "ten",   "eleven", "twelve"};
    if (n >= 0 && n < static_cast<int>(sizeof(kWords) / sizeof(kWords[0]))) return kWords[n];
    return {};
}

std::string counted(int n, std::string_view one, std::string_view many) {
    if (n == 1) return "a " + std::string(one);
    const std::string_view words = number_words(n);
    return (words.empty() ? std::to_string(n) : std::string(words)) + " " + std::string(many);
}

// A length of time as a phrase that can follow "ago" or "later": "three days", "two weeks".
// Anything under a day is handled by the callers, which have better words for it.
std::string span_words(double seconds) {
    const double days = seconds / 86400.0;
    if (days < 6.5) return counted(static_cast<int>(std::lround(days)), "day", "days");
    if (days < 11.0) return "a week";
    if (days < 25.0) return counted(static_cast<int>(std::lround(days / 7.0)), "week", "weeks");
    if (days < 45.0) return "a month";
    if (days < 330.0) return counted(static_cast<int>(std::lround(days / 30.0)), "month", "months");
    if (days < 550.0) return "a year";
    return counted(static_cast<int>(std::lround(days / 365.0)), "year", "years");
}

std::string later_words(double seconds) {
    if (seconds < 0.0) return {};
    if (seconds < 20.0 * 3600.0) return "later that day";
    if (seconds < 36.0 * 3600.0) return "the next day";
    return span_words(seconds) + " later";
}

std::string capitalised(std::string words) {
    if (!words.empty()) words[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(words[0])));
    return words;
}

bool read(double value) { return value >= 0.0; }

// The entries of one record, grouped as conversations: a summary stands alone, a gap opens
// a conversation and the spoken lines after it belong to it.
struct Talk {
    bool summary = false;
    double t = kUnread;
    std::string readings;  // the gap's, for the hour
    std::string text;      // the summary's paragraph
    std::vector<const MemoryEntry*> lines;
    int words = 0;
};

std::vector<Talk> talks_of(const std::vector<MemoryEntry>& memory, std::size_t until) {
    std::vector<Talk> talks;
    for (std::size_t i = 0; i < until && i < memory.size(); ++i) {
        const MemoryEntry& entry = memory[i];
        if (entry.kind == kMemorySummary) {
            Talk talk;
            talk.summary = true;
            talk.t = entry.t;
            talk.text = entry.text;
            talk.words = words_in(entry.text);
            talks.push_back(std::move(talk));
            continue;
        }
        if (entry.kind == kMemoryGap || talks.empty() || talks.back().summary) {
            Talk talk;
            talk.t = entry.kind == kMemoryGap ? entry.t : kUnread;
            if (entry.kind == kMemoryGap) talk.readings = entry.text;
            talks.push_back(std::move(talk));
            if (entry.kind == kMemoryGap) continue;
        }
        if (entry.kind != kMemoryPlayer && entry.kind != kMemoryNpc) continue;
        talks.back().lines.push_back(&entry);
        talks.back().words += words_in(entry.text);
    }
    return talks;
}

// Where the conversation that has just opened begins: the last gap. Everything before it
// is what the soul brings to the conversation, and everything from it on is the
// conversation itself, which the model is about to see as history anyway.
std::size_t current_gap(const std::vector<MemoryEntry>& memory) {
    for (std::size_t i = memory.size(); i > 0; --i) {
        if (memory[i - 1].kind == kMemoryGap) return i - 1;
    }
    return memory.size();
}

std::string spoken_lines(const Talk& talk, std::string_view player) {
    std::string out;
    for (const MemoryEntry* line : talk.lines) {
        out += "\n";
        out += line->kind == kMemoryPlayer ? std::string(player) : std::string("You");
        out += ": " + flat(line->text);
    }
    return out;
}

// The game's part of the day as a phrase: "in the evening", "at night".
std::string at_daypart(std::string_view daypart) {
    if (daypart.empty()) return {};
    return daypart == "night" ? std::string("at night") : "in the " + std::string(daypart);
}

// One conversation as the sheet shows it, headed by when it was and what the light was like.
std::string talk_text(const Talk& talk, const std::string& when, std::string_view player) {
    std::string head = when;
    const std::string light = at_daypart(part_of_day(hour_of(talk.readings)));
    if (!light.empty()) head += head.empty() ? capitalised(light) : ", " + light;
    if (head.empty()) head = "Another time";
    head += ", " + std::string(player) + " came to you:";
    return head + spoken_lines(talk, player);
}

}  // namespace

std::string unescape_memory(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char next = text[i + 1];
        if (next == '\\') { out.push_back('\\'); ++i; }
        else if (next == 'n') { out.push_back('\n'); ++i; }
        else if (next == 'r') { out.push_back('\r'); ++i; }
        else if (next == 'I') { out.push_back('I'); ++i; }
        else out.push_back('\\');
    }
    return out;
}

std::optional<SoulRecord> read_state_line(std::string_view line) {
    const std::size_t at = line.find(kStateHeader);
    if (at == std::string_view::npos) return std::nullopt;
    std::string_view text = line.substr(at + std::string_view("BRIDGE state ").size());
    // The game's log on Windows ends its lines CR LF, and getline takes only the LF: without
    // this the last field's value carries the CR. Harmless while the last field was a stage
    // letter or flag letters looked up by find(); the how after the slash is compared whole,
    // and "lied\r" is not "lied" -- so a scene gated on the truth was offered after a lie on
    // the Windows laptop and never on the Mac (found 8 Oct 2026 by a quest pass: --road run --how lied).
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    const Fields fields = split_fields(text, {});
    SoulRecord record;
    record.soul = std::string(fields.get("npc"));
    if (record.soul.empty()) return std::nullopt;
    record.met = count_of(fields.get("met"));
    record.first = clock_of(fields.get("first"));
    record.last = clock_of(fields.get("last"));
    record.rev = count_of(fields.get("rev"));
    record.entries = count_of(fields.get("entries"));
    record.quest = std::string(fields.get("quest"));
    if (!record.quest.empty()) {
        record.facts.accepted = fields.get("accepted") == "true";
        record.facts.handed = fields.get("handed") == "true";
        record.facts.done = fields.get("done") == "true";
    }
    // "<id>:<stage>,<id>:<stage>": one letter a stage, the furthest the quest has got, then
    // any flag letters after it, which a mod from before the flags never sends, then after a
    // slash how the favour ended, which a mod from before the scenes never sends.
    std::string_view all = fields.get("quests");
    while (!all.empty()) {
        const std::size_t comma = all.find(',');
        const std::string_view one = all.substr(0, comma);
        all = comma == std::string_view::npos ? std::string_view() : all.substr(comma + 1);
        const std::size_t colon = one.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= one.size()) continue;
        const char stage = one[colon + 1];
        const char* word = stage == 'd' ? "done" : stage == 'h' ? "handed" : stage == 'a' ? "accepted" : nullptr;
        if (word == nullptr) continue;
        record.quests.emplace_back(std::string(one.substr(0, colon)), word);
        std::string_view rest = one.substr(colon + 2);
        // The hours since the accept come last, after a tilde, and are read off first.
        if (const std::size_t tilde = rest.find('~'); tilde != std::string_view::npos) {
            if (tilde + 1 < rest.size()) {
                record.ages.emplace_back(std::string(one.substr(0, colon)),
                                         std::atof(std::string(rest.substr(tilde + 1)).c_str()));
            }
            rest = rest.substr(0, tilde);
        }
        const std::size_t slash = rest.find('/');
        if (slash != std::string_view::npos) {
            if (slash + 1 < rest.size()) {
                record.hows.emplace_back(std::string(one.substr(0, colon)), std::string(rest.substr(slash + 1)));
            }
            rest = rest.substr(0, slash);
        }
        if (!rest.empty()) {
            record.flags.emplace_back(std::string(one.substr(0, colon)), std::string(rest));
        }
    }
    for (const auto& [key, value] : fields.pairs) {
        constexpr std::string_view kStat = "stat:";
        if (key.substr(0, kStat.size()) != kStat) continue;
        record.stats.emplace_back(std::string(key.substr(kStat.size())), clock_of(value));
    }
    return record;
}

Standing standing_of(const SoulRecord& record) {
    Standing standing;
    standing.met = record.met;
    for (const auto& [id, stage] : record.quests) standing.quests[id] = stage;
    for (const auto& [id, letters] : record.flags) standing.flags[id] = letters;
    for (const auto& [id, how] : record.hows) standing.hows[id] = how;
    for (const auto& [id, hours] : record.ages) standing.ages[id] = hours;
    // The soul's own quest is in the same list when the mod is new enough to send it, and
    // on the older fields either way; the fields win, being the ones the plan reads too.
    if (!record.quest.empty()) {
        const char* stage = record.facts.done ? "done" : record.facts.handed ? "handed"
                            : record.facts.accepted ? "accepted" : nullptr;
        if (stage != nullptr) standing.quests[record.quest] = stage;
    }
    for (const auto& [name, value] : record.stats) {
        if (name == "trust") standing.trust = value;
    }
    return standing;
}

std::optional<MemoryLine> read_memory_line(std::string_view line) {
    const std::size_t at = line.find(kMemoryHeader);
    if (at == std::string_view::npos) return std::nullopt;
    std::string_view text = line.substr(at + std::string_view("BRIDGE memory ").size());
    // Only the carriage return: a remembered line may end in a space, and the log's own
    // line ending is what getline already took off.
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    const Fields fields = split_fields(text, "s=");
    MemoryLine out;
    out.soul = std::string(fields.get("npc"));
    if (out.soul.empty()) return std::nullopt;
    if (fields.has("end")) {
        out.end = true;
        return out;
    }
    for (const auto& [key, value] : fields.pairs) {
        if (value.empty() && key.find('/') != std::string_view::npos) {
            out.index = count_of(key);
            out.total = count_of(key.substr(key.find('/') + 1));
        }
    }
    out.entry.kind = std::string(fields.get("k"));
    out.entry.t = clock_of(fields.get("t"));
    out.entry.text = unescape_memory(fields.rest);
    if (out.entry.kind.empty()) return std::nullopt;
    return out;
}

std::optional<KnownLine> read_known_line(std::string_view line) {
    const std::size_t at = line.find(kKnownHeader);
    if (at == std::string_view::npos) return std::nullopt;
    std::string_view text = line.substr(at + kKnownHeader.size());
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    KnownLine out;
    std::size_t from = 0;
    while (from < text.size()) {
        const std::size_t to = text.find(' ', from);
        const std::string_view word = text.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from);
        if (!word.empty()) {
            if (out.soul.empty()) out.soul = std::string(word);
            else out.tokens.emplace_back(word);
        }
        if (to == std::string_view::npos) break;
        from = to + 1;
    }
    if (out.soul.empty()) return std::nullopt;
    return out;
}

SoulRecord record_from_lines(std::string_view text) {
    SoulRecord record;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
        if (std::optional<SoulRecord> header = read_state_line(line)) {
            record = std::move(*header);
        } else if (std::optional<MemoryLine> entry = read_memory_line(line)) {
            if (entry->soul != record.soul) continue;
            if (entry->end) record.complete = true;
            else record.memory.push_back(std::move(entry->entry));
        } else if (std::optional<KnownLine> known = read_known_line(line)) {
            if (known->soul != record.soul) continue;
            for (std::string& token : known->tokens) record.known.push_back(std::move(token));
        }
    }
    return record;
}

std::string ago_words(double seconds) {
    if (seconds < 0.0) return {};
    if (seconds < 3600.0) return "less than an hour ago";
    if (seconds < 6.0 * 3600.0) return "a few hours ago";
    if (seconds < 20.0 * 3600.0) return "earlier today";
    if (seconds < 36.0 * 3600.0) return "yesterday";
    return span_words(seconds) + " ago";
}

std::string memory_block(const SoulRecord& record, std::string_view player) {
    const std::size_t cur = current_gap(record.memory);
    const double now = cur < record.memory.size() ? record.memory[cur].t : kUnread;
    std::vector<Talk> talks = talks_of(record.memory, cur);
    if (talks.empty()) return {};

    // The newest conversation is always shown whole. Older ones are shown while there is
    // room, oldest dropped first and never one from the middle, so what remains is still a
    // run of time rather than a selection from it.
    std::size_t from = talks.size();
    int words = 0;
    bool first_raw = true;
    for (std::size_t i = talks.size(); i > 0; --i) {
        const Talk& talk = talks[i - 1];
        if (!talk.summary) {
            if (!first_raw && words + talk.words > kMemoryRawWords) break;
            first_raw = false;
            words += talk.words;
        }
        from = i - 1;
    }
    // Summaries older than the cut still count: they are the folded form of exactly the
    // lines that would otherwise have been dropped.
    std::string summaries;
    for (std::size_t i = 0; i < from; ++i) {
        if (talks[i].summary) summaries += (summaries.empty() ? "" : "\n\n") + talks[i].text;
    }

    std::string out = "You have spoken with " + std::string(player);
    const int before = record.met - 1;
    if (before == 1) out += " once before.";
    else if (before == 2) out += " twice before.";
    else if (before > 2) out += " " + counted(before, "time", "times") + " before.";
    else out += " before.";
    if (!summaries.empty()) out += "\n\n" + summaries;

    for (std::size_t i = from; i < talks.size(); ++i) {
        const Talk& talk = talks[i];
        out += "\n\n";
        if (talk.summary) {
            out += talk.text;
            continue;
        }
        const std::string when =
            read(now) && read(talk.t) ? capitalised(ago_words(now - talk.t)) : std::string();
        out += talk_text(talk, when, player);
    }

    // Said outright when the newest thing shown is a summary, whose own clock is the
    // oldest thing in it and says nothing about when they last met.
    if (talks.back().summary && read(now) && read(record.memory[cur].t)) {
        for (std::size_t i = cur; i > 0; --i) {
            const MemoryEntry& entry = record.memory[i - 1];
            if (entry.kind != kMemoryGap) continue;
            if (read(entry.t)) out += "\n\nYou last spoke " + ago_words(now - entry.t) + ".";
            break;
        }
    }
    return out;
}

std::optional<Compaction> compaction_for(const SoulRecord& record,
                                         std::string_view npc,
                                         std::string_view player) {
    const std::size_t cur = current_gap(record.memory);
    if (cur == 0 || cur >= record.memory.size()) return std::nullopt;
    const std::vector<Talk> talks = talks_of(record.memory, cur);
    int raw = 0;
    for (const Talk& talk : talks) {
        if (!talk.summary) raw += talk.words;
    }
    if (raw <= kMemoryRawWords) return std::nullopt;

    // The whole prefix, in order, with each conversation placed against the one before it
    // rather than against today, since the paragraph is going to be read for weeks.
    std::string account;
    double previous = kUnread;
    for (const Talk& talk : talks) {
        if (!account.empty()) account += "\n\n";
        if (talk.summary) {
            account += "What you already remembered:\n" + talk.text;
            if (read(talk.t)) previous = talk.t;
            continue;
        }
        std::string when;
        if (read(talk.t) && read(previous)) when = capitalised(later_words(talk.t - previous));
        else if (account.empty()) when = "One day";
        if (read(talk.t)) previous = talk.t;
        account += talk_text(talk, when, player);
    }

    Compaction compaction;
    compaction.n = static_cast<int>(cur);
    std::string system = "You keep the memories of " + std::string(npc) +
                         ", a character in a game set in Bohemia in 1403. Below is what " +
                         std::string(npc) + " remembers of earlier meetings with " +
                         std::string(player) +
                         ": perhaps a paragraph already remembered, then conversations as they "
                         "were spoken, with " +
                         std::string(npc) + "'s own lines marked \"You:\".\n\nFold all of it into "
                         "one paragraph of at most 120 words that " +
                         std::string(npc) +
                         " could be told before the next meeting. Write in the second person "
                         "to " +
                         std::string(npc) + " and in the past tense (\"" + std::string(player) +
                         " came to you while you were working and asked...\"). Keep what was "
                         "asked, promised, given, refused or learned, and what " +
                         std::string(npc) + " made of " + std::string(player) +
                         ". Do not quote lines, do not invent names or events, and write "
                         "nothing but the paragraph.";
    compaction.messages.push_back({"system", std::move(system)});
    compaction.messages.push_back({"user", std::move(account)});
    return compaction;
}

Options compaction_options(Options options) {
    // Cooler than dialogue: a summary wants the likeliest reading of what happened, not the
    // most interesting one. Long enough for the paragraph asked for and a little over.
    options.temperature = 0.3;
    options.top_p = 0.9;
    options.reply_tokens = 256;
    return options;
}

std::string summary_trouble(std::string_view summary) {
    const std::string text = flat(summary);
    if (text.empty()) return "the model wrote nothing";
    const int words = words_in(text);
    if (words >= kSummaryMostWords) return "too long at " + std::to_string(words) + " words";
    // A transcript in a paragraph's clothing: the speaker labels the account was shown.
    if (text.rfind("You: ", 0) == 0 || text.find(" You: ") != std::string::npos) {
        return "reads as a transcript, not a memory";
    }
    std::string lower;
    for (const char c : text) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower.find("as an ai") != std::string::npos ||
        lower.find("language model") != std::string::npos) {
        return "the model spoke as itself";
    }
    return {};
}

std::string compact_directive(std::string_view soul, int rev, int n, std::string_view summary) {
    const std::string text = flat(summary);
    return "@memory " + std::string(soul) + " compact " + std::to_string(rev) + " " +
           std::to_string(n) + " " + std::to_string(text.size()) + ":" + text;
}

std::string said_directive(std::string_view words) {
    const std::string text = flat(words);
    return "@said " + std::to_string(text.size()) + ":" + text;
}

std::string flat(std::string_view words) {
    std::string out;
    out.reserve(words.size());
    bool spacing = false;
    for (const char c : words) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            spacing = !out.empty();
            continue;
        }
        if (spacing) {
            out.push_back(' ');
            spacing = false;
        }
        out.push_back(c);
    }
    return out;
}

int words_in(std::string_view text) {
    int words = 0;
    bool in_word = false;
    for (const char c : text) {
        const bool blank = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!blank && !in_word) ++words;
        in_word = !blank;
    }
    return words;
}

}  // namespace moonshine
