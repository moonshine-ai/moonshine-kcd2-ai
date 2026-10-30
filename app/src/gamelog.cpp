#include "gamelog.h"

#include <cctype>
#include <cstdlib>
#include <iterator>

#include "menu.h"

namespace moonshine {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kBegin = "BRIDGE begin";
constexpr std::string_view kLook = "BRIDGE look npc=";
constexpr std::string_view kLeft = "BRIDGE left";
constexpr std::string_view kPlayed = "BRIDGE played turn=";
constexpr std::string_view kSaid = "BRIDGE said ";
constexpr std::string_view kVoice = "BRIDGE voice ";
constexpr std::string_view kLoaded = "BRIDGE state loaded";
constexpr std::string_view kReset = "BRIDGE state reset";
// The engine shutting down: "CSystem::Quit invoked from thread ... reason: ..., exit code: 0".
constexpr std::string_view kQuit = "CSystem::Quit invoked";
constexpr std::string_view kHook = "BRIDGE hook ";
constexpr std::string_view kConsole = "BRIDGE console ";
constexpr std::string_view kConsoleLine = "BRIDGE console line ";
// The menu's say row went live, and stopped being: "BRIDGE say start voice" (the talk
// button, the microphone) or "BRIDGE say start typed" (the row picked, the keyboard), then
// "BRIDGE say stop -- sent" (or back, moved, conversation over). A mod from before the two
// ways were told apart writes the start bare, which is read as voice: that mod's row took
// both. The line itself, once sent, is a said line.
constexpr std::string_view kSayStart = "BRIDGE say start";
constexpr std::string_view kSayStop = "BRIDGE say stop";
constexpr std::string_view kNpc = "npc=";
constexpr std::string_view kDoing = "doing=";

std::string_view trim(std::string_view s) {
    auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// The value of one `key=value` field, which runs to the next space. Neither soul ids nor
// look names contain one, so a field that stops there is a field that can be followed by
// another -- which is how the look came to be reported without a second marker to carry
// it. Absent rather than empty when the key is not on the line at all, because a mod too
// old to log the look is a different thing from one that looked and saw nothing.
std::string field_from(std::string_view line, std::string_view key) {
    const std::size_t at = line.find(key);
    if (at == std::string_view::npos) return {};
    std::string_view rest = trim(line.substr(at + key.size()));
    return std::string(rest.substr(0, rest.find(' ')));
}

// One numeric field, as the mod wrote it. kUnread for a field that is absent, or that the
// mod wrote as "nil" because the game would not give it up -- which is the same thing to the
// app but worth telling apart in the log. Parsed with strtod on a null-terminated copy so a
// malformed number reads as unread rather than throwing out of a log read.
double number_from(std::string_view line, std::string_view key) {
    const std::string text = field_from(line, key);
    if (text.empty()) return kUnread;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return kUnread;
    return value;
}

// The mod writes Lua's own "true"/"false".
bool flag_from(std::string_view line, std::string_view key) {
    return field_from(line, key) == "true";
}

}  // namespace

Event classify(std::string_view line) {
    // A pick's words are a row the app wrote, and the app never writes a marker into one;
    // but the words are words, so it is read before the said line for the same reason.
    if (const std::optional<Pick> pick = read_pick(line)) {
        Event event{Marker::Picked};
        event.said = pick->text;
        event.intent = pick->intent;
        event.turn = pick->row;
        return event;
    }
    // First of the rest, because the words are the player's and may say anything --
    // including the text of another marker. No other marker can contain this one: a
    // remembered line that said it arrives with the word broken as BR\IDGE, by the mod's
    // escaping.
    // Whichever marker is written first. A player can type the text of the other, and
    // a spoken line can quote it; the one that starts earlier is the one the mod sent.
    const std::size_t said_at = line.find(kSaid);
    const std::size_t voice_at = line.find(kVoice);
    if (voice_at != std::string_view::npos &&
        (said_at == std::string_view::npos || voice_at < said_at)) {
        Event event{Marker::Voice};
        const std::string_view rest = trim(line.substr(voice_at + kVoice.size()));
        const std::size_t gap = rest.find(' ');
        if (gap == std::string_view::npos || gap == 0) return {};
        event.intent = std::string(rest.substr(0, gap));
        std::string_view words = trim(rest.substr(gap + 1));
        // Outside a session the mod names the speaker (`soul=kkut_additive_man_138`)
        // ahead of the words, so the mouth can be that soul's actor's rather than the last
        // session's. Stripped here: the line's lip key is of the words alone, on both sides.
        constexpr std::string_view kSoul = "soul=";
        if (words.substr(0, kSoul.size()) == kSoul) {
            const std::size_t end = words.find(' ');
            if (end == std::string_view::npos) return {};
            event.soul = std::string(words.substr(kSoul.size(), end - kSoul.size()));
            words = trim(words.substr(end + 1));
        }
        event.said = std::string(words);
        if (event.said.empty()) return {};
        return event;
    }
    if (said_at != std::string_view::npos) {
        Event event{Marker::Said};
        event.said = std::string(trim(line.substr(said_at + kSaid.size())));
        // Enter on an empty line is not something the mod sends, but a line that has
        // been trimmed to nothing is not something worth answering either.
        if (event.said.empty()) return {};
        return event;
    }
    // A look is a begin that has not happened: the same line with the other word, and the
    // same reading of it. "npc=" is part of the marker so that a said line which happens to
    // contain the words "bridge look" is not one.
    const bool look = line.find(kLook) != std::string_view::npos;
    if (look || line.find(kBegin) != std::string_view::npos) {
        Event event{look ? Marker::Look : Marker::Begin, 0, field_from(line, kNpc),
                    field_from(line, kDoing)};
        // The raw state, judged in approach.cpp rather than here. A mod too old to report a
        // field leaves it unread, and an unread field is simply not remarked on.
        event.approach.charisma = number_from(line, "rch=");
        event.approach.blood = number_from(line, "bld=");
        event.approach.dirt = number_from(line, "drt=");
        event.approach.drink = number_from(line, "dru=");
        event.approach.health = number_from(line, "hp=");
        event.approach.armed = flag_from(line, "drawn=");
        event.approach.asleep = flag_from(line, "asleep=");
        event.approach.chatting = flag_from(line, "chatting=");
        // The world rather than either party. Both stay unread on a mod too old to send them,
        // and an unread clock is simply not mentioned -- which is the same silence as a dry sky.
        event.approach.hour = number_from(line, "hour=");
        event.approach.rain = number_from(line, "rain=");
        // The two Speech stats, for the pry (approach.h); " spc=" with its space, since nspc
        // ends in the same letters.
        const double spc = number_from(line, " spc=");
        event.approach.speech = spc < 0.0 ? -1 : static_cast<int>(spc);
        const double nspc = number_from(line, " nspc=");
        event.approach.npc_speech = nspc < 0.0 ? -1 : static_cast<int>(nspc);
        // What the player carries of the thing this soul's quest is about. A count, so it is
        // read as one; a mod with nothing to count leaves it unread.
        const double have = number_from(line, "have=");
        event.approach.have = have < 0.0 ? -1 : static_cast<int>(have);
        // Whether the player could pray at a shrine, on a pray's giver; " pray=" with its
        // space, since "prayed" is a word a said line may carry.
        const double pray = number_from(line, " pray=");
        event.approach.can_pray = pray < 0.0 ? -1 : (pray > 0.0 ? 1 : 0);
        // What Henry could give a beggar, as letters; " alms=" with its space, as on pray.
        event.approach.alms = field_from(line, " alms=");
        // Where the soul stands, "<x>,<y>" in world metres; " at=" with its space, as "at" is
        // a word. Both or neither: half a position is no position.
        const std::string at = field_from(line, " at=");
        if (const std::size_t comma = at.find(','); comma != std::string::npos) {
            char* end = nullptr;
            const double x = std::strtod(at.c_str(), &end);
            const bool x_ok = end == at.c_str() + comma;
            const char* rest = at.c_str() + comma + 1;
            const double y = std::strtod(rest, &end);
            if (x_ok && end != rest && *end == '\0') {
                event.approach.x = x;
                event.approach.y = y;
            }
        }
        // Which deal the mod was built from, for the check against this app's database.
        event.approach.quest_set = field_from(line, " qset=");
        // And the same for every quest the soul is party to, "<id>:<n>,<id>:<n>", for a soul
        // on more than one; " have=" is looked for with its space so it is not this field.
        const std::string holding = field_from(line, " holding=");
        std::string_view all = holding;
        while (!all.empty()) {
            const std::size_t comma = all.find(',');
            const std::string_view one = all.substr(0, comma);
            all = comma == std::string_view::npos ? std::string_view() : all.substr(comma + 1);
            const std::size_t colon = one.find(':');
            if (colon == std::string_view::npos || colon == 0) continue;
            const std::string count(one.substr(colon + 1));
            char* end = nullptr;
            const long n = std::strtol(count.c_str(), &end, 10);
            if (end == count.c_str()) continue;
            event.approach.holding.emplace_back(std::string(one.substr(0, colon)),
                                                static_cast<int>(n));
        }
        return event;
    }
    if (line.find(kLeft) != std::string_view::npos) {
        return {Marker::Left, 0, {}};
    }
    // The say slot: "voice" or "typed" after the start; bare is an older mod's, and voice.
    if (const std::size_t at = line.find(kSayStart); at != std::string_view::npos) {
        Event event{Marker::SayStart, 0, {}};
        const std::string_view rest = trim(line.substr(at + kSayStart.size()));
        event.said = rest == "typed" ? "typed" : "voice";
        return event;
    }
    if (const std::size_t at = line.find(kSayStop); at != std::string_view::npos) {
        Event event{Marker::SayStop, 0, {}};
        std::string_view rest = trim(line.substr(at + kSayStop.size()));
        if (rest.substr(0, 2) == "--") rest = trim(rest.substr(2));
        event.said = std::string(rest);
        return event;
    }
    // "BRIDGE hook 3 ran -- going beside Heinz", "BRIDGE hook 3 raised ...", "BRIDGE hook 3
    // did not parse: ...": the number, then whatever the mod had to say about it.
    if (const std::size_t at = line.find(kHook); at != std::string_view::npos) {
        std::string_view rest = trim(line.substr(at + kHook.size()));
        int n = 0;
        std::size_t i = 0;
        while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i]))) {
            n = n * 10 + (rest[i] - '0');
            ++i;
        }
        if (n > 0 && i > 0) {
            Event event{Marker::Hook, n, {}};
            event.said = std::string(trim(rest.substr(i)));
            return event;
        }
    }
    // "BRIDGE console line /goto 21": typed at the game's console, for the app to answer.
    // Before the plain console marker, which is a prefix of it.
    if (const std::size_t at = line.find(kConsoleLine); at != std::string_view::npos) {
        Event event{Marker::ConsoleLine, 0, {}};
        event.said = std::string(trim(line.substr(at + kConsoleLine.size())));
        return event;
    }
    if (const std::size_t at = line.find(kConsole); at != std::string_view::npos) {
        Event event{Marker::Console, 0, {}};
        event.said = std::string(trim(line.substr(at + kConsole.size())));
        return event;
    }
    // The engine's own last line, wherever the log's prefix of the day leaves it.
    if (line.find(kQuit) != std::string_view::npos) {
        return {Marker::Quit, 0, {}};
    }
    // The soul's record off the save, header then entries. Before the state chatter below,
    // since both start the same way.
    if (std::optional<SoulRecord> record = read_state_line(line)) {
        Event event{Marker::State};
        event.soul = record->soul;
        event.state = std::move(*record);
        return event;
    }
    if (std::optional<MemoryLine> entry = read_memory_line(line)) {
        Event event{Marker::Memory};
        event.soul = entry->soul;
        event.memory = std::move(*entry);
        return event;
    }
    if (std::optional<KnownLine> known = read_known_line(line)) {
        Event event{Marker::Known};
        event.soul = known->soul;
        event.known = std::move(*known);
        return event;
    }
    // The store being replaced wholesale: what follows is another world's record. The mod
    // prints the reason after the marker, and it is worth having on the status line.
    for (const std::string_view marker : {kLoaded, kReset}) {
        if (const std::size_t at = line.find(marker); at != std::string_view::npos) {
            Event event{Marker::WorldChanged};
            event.said = std::string(trim(line.substr(at + std::string_view("BRIDGE state ").size())));
            return event;
        }
    }

    const std::size_t at = line.find(kPlayed);
    if (at == std::string_view::npos) return {};

    // Parsed by hand rather than with stoi, so a marker with a malformed number is
    // ignored instead of throwing out of a log read.
    std::string_view digits = trim(line.substr(at + kPlayed.size()));
    if (digits.empty() || !std::isdigit(static_cast<unsigned char>(digits.front()))) return {};

    int turn = 0;
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) break;
        turn = turn * 10 + (c - '0');
    }
    if (turn <= 0) return {};
    return {Marker::Played, turn, {}};
}

RunCounts run_counts(const fs::path& log, std::uintmax_t limit) {
    std::ifstream in(log, std::ios::binary);
    if (!in) return {};
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(log, ec);
    if (ec) return {};
    // From no further back than the limit: a long run's log is hundreds of megabytes, and
    // the last played turn is near its end. Starting mid-line is harmless; a half line
    // cannot be mistaken for a marker with a number on it.
    if (size > limit) in.seekg(static_cast<std::streamoff>(size - limit));
    RunCounts last;
    std::string line;
    while (std::getline(in, line)) {
        // A run's log is rewritten from the start on every launch, so what is in the file
        // is one run's; the last played line in it is the mod's count now.
        const Event event = classify(line);
        if (event.marker == Marker::Played && event.turn > last.played) last.played = event.turn;
        if (event.marker == Marker::Hook && event.turn > last.hooked) last.hooked = event.turn;
        // ... unless the run is over, in which case nothing has been played in the run
        // the next launch starts.
        if (event.marker == Marker::Quit) last = {};
    }
    return last;
}

int last_played_turn(const fs::path& log, std::uintmax_t limit) {
    return run_counts(log, limit).played;
}

LogTailer::LogTailer(fs::path log) : path_(std::move(log)) {
    in_.open(path_, std::ios::binary);
    if (in_) in_.seekg(0, std::ios::end);
}

void LogTailer::reopen() {
    in_.close();
    in_.clear();
    in_.open(path_, std::ios::binary);
    tail_.clear();
}

std::uintmax_t LogTailer::position() {
    if (!in_) return 0;
    const std::streampos at = in_.tellg();
    return at < 0 ? 0 : static_cast<std::uintmax_t>(at);
}

bool LogTailer::rewound() {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path_, ec);
    return !ec && position() > size;
}

std::vector<Event> LogTailer::poll() {
    std::vector<Event> events = std::move(buffered_);
    buffered_.clear();
    std::vector<Event> more = read();
    events.insert(events.end(), std::move_iterator(more.begin()), std::move_iterator(more.end()));
    return events;
}

bool LogTailer::pending() {
    std::vector<Event> more = read();
    buffered_.insert(buffered_.end(), std::move_iterator(more.begin()), std::move_iterator(more.end()));
    return !buffered_.empty();
}

std::vector<Event> LogTailer::read() {
    std::vector<Event> events;
    if (!in_) {
        // The game may not have been running when this started.
        reopen();
        if (!in_) return events;
    }

    if (rewound()) {
        reopen();
        // First, so that everything after it is understood to belong to the new run.
        events.push_back({Marker::Restarted, 0, {}});
    }

    std::string line;
    while (std::getline(in_, line)) {
        // A final line with no newline yet is usually a line still being written, and
        // reading it now would mean reading it again when the rest arrives. But the
        // engine ends no line: it writes the newline at the front of the next one, so
        // the last thing the game said sits unterminated until it says something else --
        // minutes, between conversations (measured 5 Oct 2026: a command typed at the
        // game's console was answered only when the next line came). A fragment that has
        // not grown for kTailSettle is therefore taken as the whole line, and the newline
        // that arrives later reads as an empty line, which classifies as nothing.
        if (in_.eof()) {
            in_.clear();
            const auto now = std::chrono::steady_clock::now();
            const std::uintmax_t at = position() - line.size();
            if (!line.empty() && line == tail_ && at == tail_at_ && now - tail_since_ >= kTailSettle) {
                tail_.clear();
                Event event = classify(line);
                if (event.marker != Marker::None) events.push_back(std::move(event));
                break;
            }
            if (line != tail_ || at != tail_at_) {
                tail_ = line;
                tail_at_ = at;
                tail_since_ = now;
            }
            in_.seekg(-static_cast<std::streamoff>(line.size()), std::ios::cur);
            break;
        }
        Event event = classify(line);
        if (event.marker != Marker::None) events.push_back(std::move(event));
    }
    // getline leaves the stream unusable for the next read until it is cleared,
    // which is how following a file that is still being written works.
    in_.clear();
    return events;
}

}  // namespace moonshine
