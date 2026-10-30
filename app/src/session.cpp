#include "session.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>

#include "personadb.h"
#include "scripted.h"
#include "sentence.h"

namespace moonshine {

namespace {

// A pick the database cannot answer but knows something about, and what the model is told
// with the words: the self-account said on an earlier meeting, which the save's memory
// already carries and the model should not recite again. (Only for a soul with no line
// written for being asked again; with one, the pick is answered from it.)
constexpr std::string_view kAboutAgain =
    " [You have told them about yourself before; do not say it again word for word.]";

// Thrown out of the model's per-token callback to stop a guess part way.
struct Interrupted {};

// Whether the opening line is on disk: the soul's greeting, or the giver's own line for a
// visit the errand makes distinct. A settling turn with no line written for it is the one
// opening that has to be generated, whatever the greeting says.
bool written_opening(const Persona& persona) {
    if (persona.greeting.empty()) return false;
    return persona.quest.opening_commands.empty() || !persona.quest.greeting.empty();
}

// The bible keeps our people off Jews and Romani altogether (quest-design/bible.md §5.3),
// and the knowledge base was swept of them; a player who types the subject is turned aside
// with one of these before the model sees the words, so nothing is generated on it. The
// game's own place names (the Jewish quarter, Jewish Street) are not the subject and pass.
constexpr std::string_view kTurnedAside[] = {
    "That's not a thing I'll talk about. Was there something else?",
    "I've nothing to say on that, and I'd thank you not to ask. Anything else?",
    "Not with me, you don't. Ask me something else or be on your way.",
    "I keep my tongue off that, and so should you. What else is there?",
};

bool asks_after_a_people(std::string_view said) {
    std::string low;
    low.reserve(said.size());
    for (const char c : said) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // The place names first, so "where is Jewish Street" is a direction and not the subject.
    for (const std::string_view place : {"jewish quarter", "jewish street"}) {
        for (std::size_t at = low.find(place); at != std::string::npos; at = low.find(place)) {
            low.replace(at, place.size(), std::string(place.size(), ' '));
        }
    }
    const auto word = [&](std::size_t from, std::size_t to) {
        const bool starts = from == 0 || !std::isalpha(static_cast<unsigned char>(low[from - 1]));
        const bool ends = to >= low.size() || !std::isalpha(static_cast<unsigned char>(low[to]));
        return starts && ends;
    };
    for (const std::string_view stem : {"jew", "gyps", "romani", "roma"}) {
        for (std::size_t at = low.find(stem); at != std::string::npos; at = low.find(stem, at + 1)) {
            std::size_t end = at + stem.size();
            // The word's endings: jews, jewish, jewess, jewry; gypsy, gypsies; romani, romanies.
            while (end < low.size() && std::isalpha(static_cast<unsigned char>(low[end]))) ++end;
            const std::string_view whole(low.data() + at, end - at);
            if (!word(at, end)) continue;
            if (stem == "jew" && whole != "jew" && whole != "jews" && whole != "jewish" &&
                whole != "jewess" && whole != "jewry") {
                continue;  // jewel, jeweller
            }
            if (stem == "gyps" && whole != "gypsy" && whole != "gypsies") continue;  // gypsum
            if (stem == "romani" && whole != "romani" && whole != "romanies") continue;
            if (stem == "roma" && whole != "roma") continue;  // romance, Roman
            return true;
        }
    }
    return false;
}

}  // namespace

Session::Session(Llm& llm, TurnWriter& writer, Prefiller& prefiller, Options options)
    : llm_(llm), writer_(writer), prefiller_(prefiller), options_(std::move(options)) {}

void Session::status(std::string_view text) {
    if (listener_.on_status) listener_.on_status(text);
}

void Session::handle(const Event& event) {
    switch (event.marker) {
        case Marker::Begin:
            open(event.soul, event.approach, event.doing);
            break;
        case Marker::Look:
            look(event.soul, event.approach, event.doing);
            break;
        case Marker::Played:
            // The game's count is the one that matters, and it is allowed to be
            // ahead of what was written: a turn left over from a conversation that
            // this app did not write is still a turn the game has spent.
            if (event.turn > played_) played_ = event.turn;
            if (event.turn > written_) written_ = event.turn;
            // The words have been said and the engine has the file cached, so the
            // file itself is now only a hazard: after a relaunch the mod counts from
            // one again, and anything still on disk is a line it can say to the wrong
            // person before this app has noticed the restart and swept.
            writer_.forget(event.turn);
            status("said turn " + std::to_string(event.turn));
            break;
        case Marker::Left:
            close();
            break;
        case Marker::Said:
            // Typed in the game, so it is answered as though typed here. Said out loud
            // when there is nobody to answer, since a player who typed at a closed
            // conversation would otherwise be met with silence and no reason for it.
            if (!talking_) {
                status("the player typed \"" + event.said + "\" but nobody is listening");
                break;
            }
            status("the player typed: " + event.said);
            say(event.said, true);
            break;
        case Marker::Picked:
            // The words follow on a said line of their own and are answered there. What is
            // kept from this one is what the row was written to mean, so the director can act
            // on a row that accepts the errand without asking the model whether it did.
            // "leave" is the mod's own bottom row and ends the conversation by itself; "say"
            // is its say row, whose words are the player's own and take the typed line's road
            // (the judge, the knowledge), so no intent is kept for them.
            if (!talking_ || event.intent == "leave") break;
            if (event.intent == "say") {
                picked_intent_.clear();
                break;
            }
            picked_intent_ = event.intent;
            status("the player picked row " + std::to_string(event.turn) + " (" + event.intent +
                   "): " + event.said);
            break;
        case Marker::SayStart:
            if (!talking_) break;
            if (event.said == "typed") {
                // The row picked: the mod has the keyboard and wants nothing from here.
                status("the say row is live for typing");
                break;
            }
            set_listening(true, "the talk button is down");
            break;
        case Marker::SayStop:
            set_listening(false, event.said);
            break;
        case Marker::Restarted:
            restart();
            break;
        case Marker::State:
            // A new record replaces whatever was held, entries and all: the mod dumps the
            // whole thing every time, so there is nothing to merge.
            record_ = event.state;
            break;
        case Marker::Memory:
            // Only for the soul whose header arrived. An entry for anybody else is a dump
            // whose header was missed, and half a record is worse than none.
            if (event.memory.soul != record_.soul) break;
            if (event.memory.end) {
                record_.complete = true;
                if (static_cast<int>(record_.memory.size()) != record_.entries) {
                    status("the save promised " + std::to_string(record_.entries) +
                           " memories of " + record_.soul + " and sent " +
                           std::to_string(record_.memory.size()));
                }
                break;
            }
            record_.memory.push_back(event.memory.entry);
            break;
        case Marker::Known:
            if (event.known.soul != record_.soul) break;
            for (const std::string& token : event.known.tokens) {
                if (!record_.knows(token)) record_.known.push_back(token);
            }
            break;
        case Marker::WorldChanged:
            // Another world's save is in play, so nothing held from the last one applies.
            // Pending directives too: a compaction built from one save's record would fold
            // the wrong lines in another's, and the mod would refuse it -- but the player's
            // words are simply lost, and better lost than said to the wrong soul.
            record_ = {};
            pending_.clear();
            status("the game's store " + event.said);
            break;
        case Marker::Quit:
            // The run is over: whoever was being talked to is gone with it, and the next
            // run's log rewinding (Restarted) resets the count. A canned app stops here
            // instead (main.cpp).
            close();
            status("the game quit");
            break;
        case Marker::Hook:
            // The game ran (or tried) hook N: its count is now N, and the file is spent --
            // cached by the engine, and a hazard to the next run if left.
            if (event.turn > hooked_) hooked_ = event.turn;
            if (event.turn > hooks_written_) hooks_written_ = event.turn;
            writer_.forget_hook(event.turn);
            status("the game's hook " + std::to_string(event.turn) + " " + event.said);
            break;
        case Marker::Console:
            status("the game says: " + event.said);
            break;
        case Marker::ConsoleLine:
            // Answered in main.cpp, which has the console; nothing of the session's.
            break;
        case Marker::Voice:
            // Spoken in main.cpp, which has the speaker. Nothing about the conversation
            // changes because a line was heard.
            break;
        case Marker::None:
            break;
    }
}

void Session::open(std::string_view soul, const Approach& approach,
                   std::string_view anim) {
    // A second begin without a left is a conversation reopened, not a nested one.
    // The counter survives it; the history does not.
    set_listening(false, "a conversation opened");
    history_.clear();
    talking_ = true;
    owed_.clear();
    closing_ = false;
    dictating_ = 0;
    wrote_.clear();
    dictation_due_ = false;
    closing_words_.clear();
    henry_words_.clear();
    rows_.clear();
    drop_guesses();
    preset_.clear();
    told_.clear();
    ask_rounds_ = 0;
    fill_offset_ = 0;
    picked_intent_.clear();
    drew_.clear();
    heard_.clear();
    soul_ = std::string(soul);
    alms_ = approach.alms;
    approach_ = approach;
    anim_ = std::string(anim);

    // The look-ahead still waiting on its dwell is spent: greet() makes the greeting now.
    // And a press on somebody other than the one looked at leaves that one's greeting
    // unwanted, waiting or half made.
    look_prepare_.reset();
    if (!looked_soul_.empty() && looked_soul_ != soul && listener_.on_drop_prepared) {
        listener_.on_drop_prepared();
    }
    looked_soul_ = std::string(soul);
    persona_ = persona_for(soul, approach, anim);
    // The opening line guessed while they were looked at, if it was this soul in this very
    // state and standing -- the sheet says all of that at once -- and the model finished it.
    // One still being written when the player pressed is not waited for: it is dropped, and
    // the line generated now starts from the cache it left, which is most of the work. One
    // for another sheet is a look that came to nothing.
    bool guessed = false;
    if (greeting_guess_) {
        if (greeting_guess_->warm_only) {
            // A sheet read in ahead, or part of it; the cache has it either way.
        } else if (greeting_guess_->soul == soul && greeting_guess_->persona.sheet == persona_.sheet) {
            if (greeting_guess_->done) {
                guesses_.push_back({{}, {}, {}, greeting_guess_->reply, greeting_guess_->timing, true});
                guessed = true;
            } else {
                // With how far it got, when the backend can say: the number to read when
                // the timing line below reports a cold prefill regardless.
                const std::size_t held = llm_.held_tokens();
                const bool written = written_opening(persona_);
                status("their opening line was still being guessed when the player pressed, so " +
                       std::string(written ? "the written line is said and the guess dropped"
                                           : "it is generated from where the guess got to") +
                       " (taken up " + std::to_string(greeting_guess_->runs) + " times, stopped " +
                       std::to_string(greeting_guess_->stops) +
                       (held > 0 ? ", " + std::to_string(held) + " tokens in the cache)" : ")"));
            }
        } else if (greeting_guess_->done) {
            // Kept, not dropped: a line for another soul, or for this one in another state,
            // may still be looked for later.
            status("the opening line guessed for " + greeting_guess_->persona.name +
                   (greeting_guess_->soul == soul ? " no longer fits" : " is not wanted") +
                   ", and is kept for a later look");
            keep_greeting(std::move(*greeting_guess_));
        }
        greeting_guess_.reset();
    }
    // What the director settled before a word is said: the giver who sees the axe in the
    // player's hands pays up on this turn. Owed now so it rides on the greeting.
    for (const std::string& command : persona_.quest.opening_commands) owe(command);
    // An errand paid off on this line is not one to be reminded of (notes.md, 7 Oct 2026).
    // persona_for plans the whole menu as for an errand done when the opening completes it
    // (the topic and remarks about the thing dropped too); this holds for a persona that
    // came some other way.
    if (std::find(persona_.quest.opening_commands.begin(), persona_.quest.opening_commands.end(),
                  std::string(kQuestComplete)) != persona_.quest.opening_commands.end()) {
        persona_.scripted.remind.clear();
    }
    // The id as well as the name. They are the two halves of every question worth
    // asking about a conversation that came out wrong, and the id is the half that can
    // be looked up.
    std::string opened = "talking to " + persona_.name + " (" + std::string(soul) + ")";
    // Only when something was caught. A line reading "caught at nothing" for every soul
    // whose animation the table does not cover would bury the ones it does.
    if (!persona_.doing.empty()) opened += ", caught " + persona_.doing;
    // The quest, when there is one, because the paragraph it put in the sheet is the reason
    // the conversation goes the way it does and nothing else on the line says so.
    if (persona_.quest_of.exists()) {
        opened += persona_.quest_of.role == QuestRole::Giver ? ", who asks for " : ", who holds ";
        opened += persona_.quest_of.item_name + " (" + persona_.quest_of.id + ")";
        if (!persona_.quest.question.empty()) opened += ", listening for a yes";
    }
    // What the menu's own rows will be answered with from the database rather than the
    // model, since a reply that arrives at once and in voice is otherwise a puzzle in the log.
    {
        const Scripted& written = persona_.scripted;
        std::vector<std::string> ready;
        if (!written.offer.empty()) ready.emplace_back("the offer");
        else if (!written.help.empty()) ready.emplace_back("help");
        if (!written.about.empty()) ready.emplace_back("about");
        if (!written.topics.empty()) {
            ready.push_back(std::to_string(written.topics.size()) + " topic" +
                            (written.topics.size() == 1 ? "" : "s"));
        }
        if (!ready.empty()) {
            opened += ", with written answers for ";
            for (std::size_t i = 0; i < ready.size(); ++i) {
                if (i > 0) opened += i + 1 == ready.size() ? " and " : ", ";
                opened += ready[i];
            }
        }
    }
    // The world, for the same reason as the rest of the line: it reaches the prompt, so it is
    // part of the answer to why a conversation came out as it did, and nothing else in play
    // says whether the mod managed to read the clock at all. Silent when it did not, which is
    // the normal state for a lap after the app is rebuilt -- the mod can only be deployed
    // while the game is shut.
    for (const std::string_view reading : {part_of_day(approach.hour), sky(approach.rain)}) {
        if (!reading.empty()) opened += ", " + std::string(reading);
    }
    // What they know beyond the sheet, read in now so the first typed line does not pay for
    // it, and counted here because a soul with no knowledge base answers every question
    // from the sheet alone, which is the first thing to know when an answer is thin.
    if (knowledge_) {
        const std::size_t known = knowledge_->open(soul);
        opened += known == 0 ? std::string(", with no knowledge base")
                             : ", knowing " + std::to_string(known) + " things";
    }
    // What the save remembers, in numbers, because the memory block is the part of the
    // sheet most likely to be asked about and this is the one place its size is visible.
    if (!record_.empty()) {
        const int before = record_.met - 1;
        opened += before <= 0 ? std::string(", a first meeting")
                              : ", met " + std::to_string(before) + " time" +
                                    (before == 1 ? "" : "s") + " before";
        if (record_.memory.size() > 1) {
            opened += " (" + std::to_string(record_.memory.size() - 1) + " entries remembered)";
        }
    }
    status(opened);

    // The NPC speaks first either way: a player who pressed the button should not have to
    // open the conversation themselves.
    //
    // Except at the moment the quest is paid off. The state greetings were composed for a
    // stranger at the door; "How can I serve, sir?" from a man who has just seen his own axe
    // come back is wrong enough to break the scene. The writer gives the giver a line for
    // that moment too (his thanks, or the line for a thing brought unasked), and that is
    // said; only a giver with none written is left to the model, slow path and all.
    // A fresh line guessed for this moment comes before a written one: it was written for
    // this moment too, and by the same model that will have to answer what follows it.
    if (guessed) {
        status("their opening line was guessed while the player looked at them, and is said at once");
        answer("");
        return;
    }
    if (written_opening(persona_)) {
        if (!persona_.quest.greeting.empty()) {
            // The visit's own line, whichever the source put in `greeting`: the two are the
            // same when the source is persona_for, and this is what makes them so otherwise.
            persona_.greeting = persona_.quest.greeting;
            status(persona_.quest.opening_commands.empty()
                       ? "the errand is open between them, so the opening line is the one written for that visit"
                       : "the quest settles on this turn, on the line written for it");
        }
        // An errand paid off on the greeting ends the same way a hand-in row ends one, and
        // leads on the same way: where another errand of this giver's follows, its offer is
        // said with the thanks and its accepting row goes first on the list (notes.md, 9 Oct
        // 2026 -- Benesh took his chalice back on sight and the letter for Father Adam was
        // never offered; the run's "Return to Benesh" stood and the entry never closed).
        bool leads_on = false;
        if (std::find(persona_.quest.opening_commands.begin(), persona_.quest.opening_commands.end(),
                      std::string(kQuestComplete)) != persona_.quest.opening_commands.end()) {
            Settled ending = settle(kQuestComplete);
            leads_on = ending.leads_on;
            if (leads_on) {
                const Scripted& coming = ending.next.scripted;
                // The thanks written for leading on, where there are some: the ones that
                // do not close anything (notes.md, 13 Oct 2026 -- Benesh thanked Henry as
                // if the matter were done, and then asked the next thing).
                if (!persona_.quest_of.thanks_onward.empty()) persona_.greeting = persona_.quest_of.thanks_onward;
                persona_.greeting += "\n\n" + coming.offer;
                preset_.clear();
                if (!coming.accept_row.empty()) preset_.push_back({0, "accept", coming.accept_row});
                if (!coming.more_row.empty()) preset_.push_back({0, "more", coming.more_row});
                if (!coming.more_2_row.empty()) preset_.push_back({0, "more_2", coming.more_2_row});
                told_.emplace_back("help");
            } else if (ending.ended) {
                // Paid off on sight with nothing to lead on to: the thanks close the talk,
                // as a hand-in row's do (answer_scripted).
                closing_ = true;
            }
            // Before the greeting goes out, so the list under it is the adopted plan's.
            adopt(std::move(ending));
        }
        greet();
        if (leads_on && !drew_.empty()) drew_.back().item_id = "quest:" + persona_.quest_of.id;
        return;
    }
    if (!persona_.quest.opening_commands.empty()) {
        status("the quest settles on this turn and no line is written for it, so the opening line is generated");
    }

    // Nothing written for them, so it has to be generated, and that is the slow path.
    // Warming belongs on the approach, before the player chooses to talk; doing it here
    // would only prefill the very prompt that is about to be sent, and that pays for
    // itself. All this can usefully do is say that the wait is expected.
    if (!prefiller_.is_warm(persona_)) {
        status(persona_.name + " was not warmed, so this conversation opens slowly");
    }
    answer("");
}

Persona Session::persona_for(std::string_view soul, const Approach& approach,
                             std::string_view anim) {
    // The record the mod dumped just before this line, if it is this soul's. A record for
    // somebody else is one from a mod that did not dump for this soul -- or an older
    // conversation's, kept past a save the mod did not announce -- and is dropped rather
    // than believed. The mod prints the dump before the line, so there is nothing to wait
    // for: a record that is not here now is not coming.
    if (record_.soul != soul) {
        if (!record_.empty()) {
            status("the save's record is of " + record_.soul + ", not " + std::string(soul) +
                   ", so it is not used");
        }
        record_ = {};
    } else if (!record_.complete) {
        status("the save's record of " + record_.soul + " arrived without its end");
    }
    std::optional<Persona> found = find_ ? find_(soul, approach, anim, record_) : std::nullopt;
    return found ? *found : stranger();
}

// The player has a soul in their sights from close enough to talk, and has not pressed.
//
// What follows a press is open(), and the one slow thing open() can do is generate the
// opening line: a soul with nothing written for them, or a giver on the turn the quest
// settles. This is where that wait goes instead. The line is planned here and generated by
// speculate() from the poller's idle loop, stopped within a token (or a piece of the
// prefill) by anything the game says next, and said at once by open() if the sheet it was
// generated against is the sheet open() builds. The written greeting knows nothing of the
// state the player walks up in -- bloody, armed, at midnight -- so the guess is a fresher
// line than the written one and worth the poller's idle time; only a visit with its own
// line written (the giver seeing his axe come back) need not trouble the model.
void Session::look(std::string_view soul, const Approach& approach, std::string_view anim) {
    if (talking_) return;
    Persona persona = persona_for(soul, approach, anim);
    const bool same = greeting_guess_ && greeting_guess_->soul == soul &&
                      greeting_guess_->persona.sheet == persona.sheet;
    // The opening list is all written. Sent now, as the greeting's next turn, so the menu
    // is in the inbox before the press and the engine's "..." is covered as soon as it
    // appears (notes.md, 5 Oct 2026). offer() rewrites the same turn on the press.
    if (menu_ && !same) preview_opening_list(persona);
    // Whether or not a line is guessed for them: a guess still being made when the player
    // presses gives way to the written line, and that is the usual press -- a few seconds
    // after the look, with the sheet still being read in. Not started here, though: the
    // voice's part is planned for speculate() to start once the look has lasted
    // (look_dwell_), and a look at somebody else before then replaces it. The one before
    // it is not wanted any more, whether it is waiting or being made.
    if (!same) {
        if (!looked_soul_.empty() && looked_soul_ != soul && listener_.on_drop_prepared) {
            listener_.on_drop_prepared();
        }
        looked_soul_ = std::string(soul);
        look_due_ = std::chrono::steady_clock::now() + look_dwell_;
        look_prepare_.reset();
        if (listener_.on_prepare && written_opening(persona)) {
            look_prepare_ = PendingPrepare{beats_of(persona.greeting), persona.actor};
        }
    }
    if (!speculate_) return;
    // The same soul in the same state as the guess in hand: nothing to do, whether it is
    // finished or still being written.
    if (same) return;
    // A finished line is kept rather than dropped: the look may come back to them.
    if (greeting_guess_ && greeting_guess_->done && !greeting_guess_->warm_only) {
        keep_greeting(std::move(*greeting_guess_));
    }
    greeting_guess_.reset();

    std::string seen = "looked at " + persona.name + " (" + std::string(soul) + ")";
    // Off the record, as the persona source reads it: the begin line's approach does not
    // say, and the mod counts the conversation about to open in its dump.
    seen += record_.met > 1 ? ", met before" : ", a first meeting";
    if (std::optional<GreetingGuess> kept = take_kept_greeting(soul, persona.sheet)) {
        status(seen + "; their opening line was guessed on an earlier look, and is kept");
        greeting_guess_ = std::move(*kept);
        return;
    }
    if (persona.greeting_exact && written_opening(persona)) {
        // The written line is said on the press. But the sheet is worth having in the cache
        // before then: the reply to the first pick prefills it otherwise, after the written
        // line has gone out, and that is two seconds of the poller's time the look can pay.
        if (prefiller_.is_warm(persona)) return;
        status(seen + "; a line is written for this, so only their sheet is read in ahead");
        greeting_guess_ = GreetingGuess{std::string(soul), std::move(persona), {}, {}, false, true};
        return;
    }
    const std::string why = persona.greeting.empty() ? "nothing is written for them"
                            : !persona.quest.opening_commands.empty() ? "the quest settles on the line"
                            : "the written line knows nothing of the state they are met in";
    status(seen + "; " + why + ", so their opening line is guessed ahead");
    greeting_guess_ = GreetingGuess{std::string(soul), std::move(persona), {}, {}, false, false};
}

std::string Session::help_row() const {
    return persona_.scripted.help_row.empty() ? std::string(kFirstRoundSeeds[0])
                                              : persona_.scripted.help_row;
}

bool Session::knows_of(const Topic& topic) const {
    if (topic.about.empty()) return true;
    if (topic.about == soul_) return true;
    if (record_.knows(topic.about)) return true;
    return std::find(heard_.begin(), heard_.end(), topic.about) != heard_.end();
}

bool Session::ask_due() const {
    for (const Topic& topic : persona_.scripted.topics) {
        if (knows_of(topic)) return true;
    }
    return false;
}

bool Session::nothing_to_ask() const {
    // A soul nothing was written for keeps the row: the model answers it as it always did.
    // One with things written, all of them held back -- by trust, or about people Henry
    // has not heard of -- has nothing behind the question, so it is not asked.
    const Scripted& written = persona_.scripted;
    return (written.withheld || !written.topics.empty()) && !ask_due();
}

void Session::note_heard(std::string_view words) {
    for (const std::string& token : people_named(words, persona_.scripted.people)) {
        if (std::find(heard_.begin(), heard_.end(), token) != heard_.end()) continue;
        heard_.push_back(token);
        if (!record_.knows(token)) pending_.push_back(std::string(kHeardDirective) + " " + token);
    }
}

void Session::preview_opening_list(const Persona& persona) {
    Persona previous = std::move(persona_);
    const auto told = told_;
    const int fill = fill_offset_;
    auto held = std::move(rows_);
    auto heard = std::move(heard_);
    const auto pending = pending_.size();
    persona_ = persona;
    told_.clear();
    fill_offset_ = 0;
    rows_.clear();
    // The list follows the greeting, so whoever the greeting names is heard of by then.
    // Nothing is owed the save for it here: greet() reads the same line on the press.
    heard_.clear();
    note_heard(persona_.greeting);
    pending_.resize(pending);

    int reserved = 0;
    for (const DirectorRow& row : persona_.quest.rows) {
        if (row.after_yes && !persona_.quest.question.empty()) continue;
        if (std::find(told_.begin(), told_.end(), row.intent) == told_.end()) ++reserved;
    }
    if (!persona_.quest.apologise_row.empty()) ++reserved;
    std::vector<MenuOption> seeds = first_round_rows();
    if (!persona_.scripted.help_due && persona_.scripted.remind.empty()) {
        seeds.erase(std::remove_if(seeds.begin(), seeds.end(),
                                   [](const MenuOption& row) { return row.intent == "help"; }),
                    seeds.end());
    }
    for (MenuOption& row : seeds) {
        if (row.intent != "help") continue;
        if (!persona_.scripted.remind.empty()) {
            row.intent = "remind";
            row.text = std::string(kRemindRow);
        } else {
            row.text = help_row();
        }
    }
    if (nothing_to_ask()) {
        seeds.erase(std::remove_if(seeds.begin(), seeds.end(),
                                   [](const MenuOption& row) { return row.intent == "ask"; }),
                    seeds.end());
    }
    const std::size_t keep = static_cast<std::size_t>(std::max(1, kMenuOptions - reserved));
    if (seeds.size() > keep) seeds.resize(keep);
    for (MenuOption& option : seeds) {
        option.row = static_cast<int>(rows_.size()) + 1;
        rows_.push_back(std::move(option));
    }
    const DirectorRow* give_up = nullptr;
    for (const DirectorRow& row : persona_.quest.rows) {
        if (row.intent == "give_up") give_up = &row;
    }
    const int room = kMenuOptions - (give_up ? 1 : 0);
    if (!persona_.quest.apologise_row.empty() && static_cast<int>(rows_.size()) < room) {
        rows_.push_back({static_cast<int>(rows_.size()) + 1, "apologise", persona_.quest.apologise_row});
    }
    for (const DirectorRow& row : persona_.quest.rows) {
        if (&row == give_up) continue;
        if (row.after_yes && !persona_.quest.question.empty()) continue;
        if (static_cast<int>(rows_.size()) >= room) break;
        rows_.push_back({static_cast<int>(rows_.size()) + 1, row.intent, row.text});
    }
    if (std::vector<MenuOption> fill_rows = written_fill(true, room - static_cast<int>(rows_.size()));
        !fill_rows.empty()) {
        for (MenuOption& option : fill_rows) {
            option.row = static_cast<int>(rows_.size()) + 1;
            rows_.push_back(std::move(option));
        }
    }
    // The give-up row last, in the place kept for it (notes.md 13 Oct 2026).
    if (give_up != nullptr && static_cast<int>(rows_.size()) < kMenuOptions) {
        rows_.push_back({static_cast<int>(rows_.size()) + 1, give_up->intent, give_up->text});
    }
    // After the greeting, which takes a turn per beat (write_beats).
    const int turn = next_turn() + static_cast<int>(beats_of(persona_.greeting).size());
    if (writer_.write(turn, whole_list_turn(rows_))) {
        status("opening rows written ahead for " + persona_.name + ", so the menu is ready on the press");
    }
    persona_ = std::move(previous);
    told_ = told;
    fill_offset_ = fill;
    rows_ = std::move(held);
    heard_ = std::move(heard);
}

void Session::prepare_greeting(const Persona& persona) {
    if (!listener_.on_prepare || !written_opening(persona)) return;
    // A beat each, since each is a turn of its own and the mod asks for them one at a time.
    // The directives write_beats puts in front are the mod's, taken off before it speaks.
    for (const std::string& beat : beats_of(persona.greeting)) {
        listener_.on_prepare(beat, persona.actor);
    }
}

// Say the line that was written for them ahead of time.
//
// This is the whole reason the generator writes greetings. Generating the opening line
// means prefilling the world and the sheet before a single word can be spoken, which cost
// 15.15s in the first real conversation -- the player stands in front of somebody who says
// nothing. A written greeting is on disk already, so it goes to the game at once, and the
// prefill for the reply happens while the player is still reading it.
void Session::greet() {
    const int turn = (played_ > written_ ? played_ : written_) + 1;
    note_heard(persona_.greeting);
    const int last = write_beats(turn, persona_.greeting);
    if (last == 0) {
        status("turn " + std::to_string(turn) + " could not be written");
        return;
    }
    written_ = last;
    owed_.clear();
    pending_.clear();
    // The words are on disk; the voice can start on them now rather than when the mod has
    // shown them. Already made when a look came first, and then this costs nothing.
    prepare_greeting(persona_);

    if (listener_.on_say) listener_.on_say(persona_.greeting);
    // In the history as though it had been generated, because as far as the conversation
    // is concerned it was said, and the model has to answer as the person who said it.
    history_.push_back({"assistant", persona_.greeting});
    if (listener_.on_turn) listener_.on_turn(history_.back());
    drew({}, persona_.greeting);

    // The rows, which do not need the prefill: if the look did not write them ahead the
    // player would sit on the engine's "..." until the sheet was read (notes.md, 5 Oct
    // 2026 -- a second or two of dots on CPU). The prefill their reply will need is not
    // done here. It used to be, "while they are still reading the greeting" -- but the
    // greeting is now spoken, and the voice starts on the mod's played marker, which the
    // poller reads only once this call returns; a sheet read here held that marker for
    // the length of the read, 6 s on the Windows laptop with the model standing aside for the voice's
    // own work, and the player heard that as a greeting that would not start (12 Oct
    // 2026). It is speculation now (has_speculation, speculate): the poller does it
    // between looks at the log, and gives it up the moment a line arrives.
    offer();
}

void Session::close() {
    if (!talking_) return;
    // A say row still live has nothing to be live for.
    set_listening(false, "the conversation ended");
    talking_ = false;
    closing_words_.clear();
    henry_words_.clear();
    history_.clear();
    rows_.clear();
    drop_guesses();
    preset_.clear();
    told_.clear();
    ask_rounds_ = 0;
    fill_offset_ = 0;
    picked_intent_.clear();
    drew_.clear();
    status("conversation over");
    // Planned now, with the player walking away, and done between looks at the log rather
    // than here, where it would hold the next begin. The record it judges is the one
    // dumped before this conversation; the lines just said are not in it, and are not
    // what needs folding anyway -- the newest conversation is always kept whole.
    compact();
    record_ = {};
}

void Session::compact() {
    std::optional<Compaction> due = compaction_for(record_, persona_.name, kPlayer);
    if (!due) return;
    if (fold_ && fold_->soul != record_.soul) {
        status("the fold of " + fold_->name + "'s " + std::to_string(fold_->due.n) +
               " entries is set aside, not done in time; their next conversation asks again");
    }
    fold_ = PendingFold{record_.soul, persona_.name, record_.rev, std::move(*due)};
    status(std::to_string(fold_->due.n) + " remembered entries of " + persona_.name +
           " are due to be folded into a paragraph, between looks at the log");
}

bool Session::fold(const std::function<bool()>& interrupted) {
    if (!fold_) return false;
    PendingFold& fold = *fold_;
    std::string summary;
    try {
        llm_.chat(fold.due.messages, compaction_options(options_), [&](std::string_view delta) {
            summary += delta;
            if (interrupted()) throw Interrupted{};
        });
    } catch (const Interrupted&) {
        // Taken up again next time round, from the start; the prompt is the same, so the
        // backend has most of it cached.
        ++fold.stops;
        return true;
    } catch (const std::exception& e) {
        status(std::string("the model could not be reached to fold them: ") + e.what());
        fold_.reset();
        return false;
    }
    if (const std::string trouble = summary_trouble(summary); !trouble.empty()) {
        // Nothing is lost by refusing: the lines stay as they were and the next conversation
        // asks again. A bad paragraph kept would be a memory nobody can take back.
        status("the paragraph was refused (" + trouble + "), so nothing was folded");
        fold_.reset();
        return false;
    }
    // Rides on the first turn file of the next conversation, whoever it is with: the
    // directive names the soul, and the mod checks the revision it was built from.
    pending_.push_back(compact_directive(fold.soul, fold.rev, fold.due.n, summary));
    status("folded " + std::to_string(fold.due.n) + " remembered entries of " + fold.name + " into " +
           std::to_string(words_in(summary)) + " words, for the next turn to carry" +
           (fold.stops > 0 ? " (stopped " + std::to_string(fold.stops) + " times by the log)" : ""));
    fold_.reset();
    return false;
}

void Session::say(std::string_view said, bool heard_by_game) {
    if (!talking_) return;
    // Nothing to answer. An empty transcript is what silence or a failed
    // transcription looks like, and answering it would spend a turn on nothing.
    if (said.empty()) return;
    // The mod has these on the screen and in Henry's voice (BRIDGE voice henry) as this
    // arrives, and holds the camera on him until spoken() hears that voice out.
    henry_words_ = std::string(said);
    answer(said, !heard_by_game);
}

std::string Session::with_owed(std::string_view words, bool closing) {
    // On the same line as the words, because the writer flattens a turn to one line and the
    // mod reads one line. Three tokens a quest directive, a counted text for the others, and
    // the mod takes them all off the front before it speaks what is left.
    std::string out;
    for (const std::string& command : owed_) {
        out += quest_directive(owed_for_, command) + " ";
    }
    for (const std::string& directive : pending_) out += directive + " ";
    // The errand taken up closes the conversation once the giver's answer has been read,
    // as the game's own quests close on the accepting line (noted 4 Oct 2026): the mod
    // leaves after the reading hold and the accept beat, and offers no menu.
    //
    // A list of rows is not a line and is not spoken, and the mod takes it off the hold by
    // its first token, so nothing goes in front of it but what is owed.
    const bool rows = words.substr(0, 5) == "@opt " || words.substr(0, 6) == "@rows ";
    if (!rows) {
        out += line_directives(closing_ && closing);
        // The whole line, not an update of it: @more carries words still being written.
        if (closing_ && closing && words.substr(0, 5) != "@more") closing_words_ = std::string(words);
    }
    return out + std::string(words);
}

std::string Session::line_directives(bool closing) const {
    std::string out;
    if (closing) out += std::string(kEndDirective) + " ";
    if (voiced_) out += std::string(kVoicedDirective) + " ";
    return out;
}

// The words as the mod shows them and the voice marker carries them: one line, each run of
// whitespace a single space.
std::string Session::flat_words(std::string_view text) {
    std::string flat;
    flat.reserve(text.size());
    bool spacing = false;
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            spacing = !flat.empty();
            continue;
        }
        if (spacing) flat.push_back(' ');
        spacing = false;
        flat.push_back(c);
    }
    return flat;
}

void Session::set_listening(bool on, std::string_view why) {
    if (listening_ == on) return;
    heard_text_.clear();
    heard_complete_ = false;
    heard_at_ = {};
    if (!on) {
        listening_ = false;
        status("stopped listening: " + std::string(why));
        if (listener_.on_listen) listener_.on_listen(false, {});
        return;
    }
    // The microphone opens with the conversation so far as its context, or does not open
    // at all -- a build without an ear, no microphone -- in which case the mod is told, so
    // the slot it has just shown takes the keyboard rather than wait on words that will
    // never come.
    const bool open = listener_.on_listen && listener_.on_listen(true, listen_context());
    if (!open) {
        status("cannot listen: no ear; the say slot takes the keyboard");
        hook("Moonshine.SayDeaf()", "no ear");
        return;
    }
    listening_ = true;
    status("listening: " + std::string(why));
}

std::string Session::listen_context() const {
    // The soul's name first, said twice over so a one-word passage still has a term in it
    // to find, then the lines in the order they were said. The transcriber picks the
    // unusual words out itself (the proper nouns, in effect), so the ordinary words
    // around them cost nothing but the read.
    std::string passage;
    if (!persona_.name.empty()) passage = persona_.name + ". " + persona_.name + ".";
    for (const Message& message : history_) {
        if (message.role != "assistant" && message.role != "user") continue;
        if (message.content.empty()) continue;
        if (!passage.empty()) passage += '\n';
        passage += message.content;
    }
    // And the rows on offer, which are the lines the player is most likely to be
    // paraphrasing out loud.
    for (const MenuOption& row : rows_) {
        if (row.text.empty()) continue;
        if (!passage.empty()) passage += '\n';
        passage += row.text;
    }
    return passage;
}

// Lua's reading of the words, between double quotes: the backslash, the quotes and the
// line breaks are all the text could carry that the chunk would misread. Everything else
// goes through as bytes, which is what the slot shows.
static std::string lua_quoted(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            default: out.push_back(c);
        }
    }
    return out + "\"";
}

void Session::heard(std::string_view text, bool complete) {
    if (!listening_ || !talking_) return;
    const std::string flat = flat_words(text);
    // Nothing heard is nothing to send, finished or not.
    if (flat.empty()) complete = false;
    if (flat == heard_text_ && complete == heard_complete_) return;
    // The flag turning true is the player's line going on, and is not held back; the words
    // changing is kept to the mod's pace.
    const bool finishing = complete && !heard_complete_;
    const auto now = std::chrono::steady_clock::now();
    if (!finishing && heard_at_ != std::chrono::steady_clock::time_point{} &&
        now - heard_at_ < kHeardEvery) {
        return;
    }
    heard_text_ = flat;
    heard_complete_ = complete;
    heard_at_ = now;
    hook("Moonshine.SayHeard(" + lua_quoted(flat) + (complete ? ", true)" : ", false)"),
         std::string(complete ? "heard (done): " : "heard: ") + flat);
}

void Session::spoken(std::string_view text) {
    if (!talking_ || !voiced_) return;
    const std::string flat = flat_words(text);
    // A hook rather than a turn, because a turn has a place in the order of what is said
    // and this does not: Henry's reply is usually written before his own line has finished
    // playing, and a turn saying so behind it would be read after it.
    if (!henry_words_.empty() && flat == flat_words(henry_words_)) {
        henry_words_.clear();
        if (hook("Moonshine.Spoken(\"henry\")", "Henry's line spoken")) {
            status("Henry's line has been spoken; the camera may move on");
        }
        return;
    }
    if (!closing_words_.empty() && flat == flat_words(closing_words_)) {
        closing_words_.clear();
        if (hook("Moonshine.Spoken(\"npc\")", "the closing line spoken")) {
            status("the closing line has been spoken to the end; the mod may leave");
        }
    }
}

int Session::write_beats(int turn, std::string_view reply) {
    // A written line may be told in two beats -- a widow's account of why she wants the
    // flowers, then what she asks (notes.md 7 Oct 2026) -- so the player is not handed a
    // paragraph. Each beat is a turn the mod holds for its reading; the @beat flag on all
    // but the last keeps the menu shut between them.
    const std::vector<std::string> beats = beats_of(reply);
    for (std::size_t i = 0; i < beats.size(); ++i) {
        const bool last = i + 1 == beats.size();
        std::string line = last ? std::string() : std::string(kBeatDirective) + " ";
        if (i == 0) {
            line += with_owed(beats[i], last);
        } else {
            line += line_directives(last && closing_);
            if (last && closing_) closing_words_ = beats[i];
            line += beats[i];
        }
        if (!writer_.write(turn + static_cast<int>(i), line)) return 0;
    }
    return turn + static_cast<int>(beats.size()) - 1;
}

void Session::owe(std::string_view command) {
    if (!persona_.quest_of.exists()) return;
    owed_for_ = persona_.quest_of;
    owed_.emplace_back(command);
    // Taken up, or taken up again at the chest (the yes that names the house-girl; notes.md
    // 14 Oct 2026): the giver's answer is the last word and the conversation closes on it.
    // Not a letter to be written at dictation: taking that up is the start of the writing,
    // and the dictation is due in this conversation (the accepting row begins it; a yes the
    // judge read lays the row that picks it up). Its sealing line is what closes instead.
    // Nor while a row laid for after the yes waits (the giver's carrying, DirectorRow::
    // after_yes): the next list is those rows, and they close it themselves.
    const bool after_yes = std::any_of(persona_.quest.rows.begin(), persona_.quest.rows.end(), [this](const DirectorRow& row) {
        return row.after_yes && std::find(told_.begin(), told_.end(), row.intent) == told_.end();
    });
    if (command == kQuestAccept && persona_.quest_of.has_dictation()) {
        if (dictating_ == 0) dictation_due_ = true;
    } else if (command == kQuestAccept && after_yes) {
        status("the yes leaves the conversation open for the rows laid after it");
    } else if (command == kQuestAccept || command == kQuestNamed) {
        closing_ = true;
    }
    if (command.substr(0, kQuestWrote.size()) == kQuestWrote) closing_ = true;
    if (command == kQuestFastTravel) closing_ = true;
    status("quest " + persona_.quest_of.id + ": " + std::string(command));
    if (listener_.on_quest) listener_.on_quest(persona_.quest_of.id, command);
}

std::string Session::judge(std::string_view said) {
    if (said.empty()) return {};
    // A row of the director's with no answer written for it (the give-up row on most
    // errands, the chest taken up again without a `named_reply`): the model answers it, in
    // the person's own way, told what has just become true; the command still goes out
    // with the reply, and an ending is settled the same as on a written one.
    if (!picked_intent_.empty()) {
        for (const DirectorRow& row : persona_.quest.rows) {
            if (row.intent != picked_intent_ || !row.reply.empty()) continue;
            if (std::find(told_.begin(), told_.end(), row.intent) != told_.end()) break;
            status("picked as " + picked_intent_ + ", answered by the model with the command owed");
            if (!row.command.empty()) owe(row.command);
            told_.push_back(row.intent);
            if (row.closes) closing_ = true;
            if (row.command == kQuestAbandon) {
                for (const DirectorRow& other : persona_.quest.rows) told_.push_back(other.intent);
                told_.emplace_back("remind");
                persona_.quest.question.clear();
                settling_ = settle(row.command);
            }
            return {};
        }
    }
    if (persona_.quest.question.empty()) return {};
    // Words picked off the menu came with their meaning attached. A row that accepts is
    // a yes without asking. A row written to question, press, joke or leave was not written
    // to accept, so there is nothing to ask. "agree" is agreement to whatever was last said
    // and may well be the errand, and "help" is the row that goes along with it: the first
    // in-game pass had the model write "I'll see what I can do about the axe" as `help`
    // the round after the offer, and the errand went untaken. Both go to the judge like
    // typed words -- except the opening round's own "Is there anything I can help you
    // with?", which the app wrote, before any offer, and which is not an acceptance.
    if (!picked_intent_.empty()) {
        if (picked_intent_ == "accept") {
            status("picked as accept, so the judge was not asked");
            owe(persona_.quest.on_yes);
            persona_.quest.question.clear();
            return persona_.quest.direction;
        }
        const bool could_be_a_yes = picked_intent_ == "agree" || picked_intent_ == "help";
        if (!could_be_a_yes || said == kFirstRoundSeeds[0] || said == help_row()) {
            status("picked as " + picked_intent_ + ", so the judge was not asked");
            return {};
        }
        status("picked as " + picked_intent_ + ", which may be a yes; asking the judge");
    }
    // The same conversation the reply is about to be asked for, with the question under
    // the player's words. The whole prefix is shared, so what this costs is the question and
    // a few tokens of answer -- measured on the way in rather than assumed, see on_timing.
    std::string verdict_text;
    try {
        llm_.chat(judge_messages(conversation(world_, persona_, history_, said),
                                 persona_.quest.question),
                  judge_options(options_),
                  [&](std::string_view delta) { verdict_text += delta; });
    } catch (const std::exception& e) {
        status(std::string("the judge could not be reached: ") + e.what());
        return {};
    }
    const Verdict verdict = read_verdict(verdict_text);
    status("judged \"" + std::string(said).substr(0, 40) + "\" as " +
           (verdict == Verdict::Yes ? "yes" : verdict == Verdict::No ? "no" : "unclear") +
           " (" + verdict_text + ")");
    if (verdict != Verdict::Yes) return {};
    owe(persona_.quest.on_yes);
    // Asked once. What it asked about is now true and stays true for the conversation.
    persona_.quest.question.clear();
    return persona_.quest.direction;
}

void Session::drew(std::string item_id, std::string_view reply) {
    drew_.push_back({std::move(item_id), std::string(reply)});
    // The follow-up reading looks three replies back at most.
    constexpr std::size_t kKeep = 8;
    if (drew_.size() > kKeep) drew_.erase(drew_.begin());
}

bool Session::offers_help(std::string_view said) {
    if (!knowledge_ || said.empty()) return false;
    float score = 0.0f;
    const bool offering = knowledge_->is_quest_ask(said, &score);
    if (offering) {
        char scored[16];
        std::snprintf(scored, sizeof(scored), "%.2f", static_cast<double>(score));
        status("\"" + std::string(said).substr(0, 40) + "\" offers help (" + scored +
               "), so the quest comes before anything they know");
    }
    return offering;
}

AskState Session::ask_state() const {
    if (!persona_.quest_of.exists() || persona_.quest_of.role != QuestRole::Giver) {
        return AskState::None;
    }
    // While the director is still listening for a yes, the favour is there to be asked;
    // once it has stopped, the favour was taken up or was never offerable this visit.
    if (persona_.quest.question.empty()) return AskState::Settled;
    const bool asked = std::find(told_.begin(), told_.end(), "help") != told_.end() ||
                       std::any_of(drew_.begin(), drew_.end(), [](const Drew& d) {
                           return d.item_id.rfind("quest:", 0) == 0;
                       });
    return asked ? AskState::Again : AskState::Fresh;
}

Advice Session::advise(std::string_view said, bool offering) {
    if (!knowledge_ || said.empty()) return {};
    const Quest& quest = persona_.quest_of;
    if (offering) {
        return quest_ask_direction(quest.id, quest.offer, quest.reward_groschen, ask_state());
    }
    // What they know is the help their favour pays for, and is held back until it is done
    // (KbGate): a giver asked a question puts it off and asks their favour instead, or
    // reminds the player of the one taken up.
    const Scripted& written = persona_.scripted;
    const bool giver = quest.exists() && quest.role == QuestRole::Giver;
    return gate_knowledge(knowledge_->advise(soul_, said, written.tier, drew_), written.kb_gate,
                          giver ? quest.id : std::string(), giver ? quest.offer : std::string(),
                          ask_state() == AskState::Again, written.remind);
}

void Session::answer(std::string_view said, bool remember) {
    // A typed line that offers help is the menu's own `help` row in other words, and is
    // answered as that row would be: from the written offer where there is one, with the
    // accepting row laid after it, so the errand is taken on a pick. Where nothing is
    // written, or it has been said, the model answers it with the ask as its direction.
    // Words typed or spoken on a subject the bible keeps us off are turned aside in a
    // written line, and the model is not asked; the words still go to the save, and to the
    // history, so the person goes on as somebody who was asked and would not say.
    if (picked_intent_.empty() && asks_after_a_people(said)) {
        unsigned which = static_cast<unsigned>(history_.size());
        for (const char c : soul_) which = which * 31u + static_cast<unsigned char>(c);
        const std::string_view line = kTurnedAside[which % std::size(kTurnedAside)];
        status("the line asks after a people the bible keeps us off, so it is turned aside as written");
        if (say_written(said, std::string(line), remember)) offer();
        return;
    }
    const bool offering = picked_intent_.empty() && offers_help(said);
    if (offering && written_answer("help")) picked_intent_ = "help";

    // A pick with an answer already written for it is said from the database, at once and
    // in voice, and the model is not asked. Everything else, and every pick nothing was
    // written for, goes on to the model as it always did.
    if (!picked_intent_.empty() && answer_scripted(said, remember)) {
        picked_intent_.clear();
        return;
    }
    // A dictation row out of its turn (a part not being chosen on, a way not written for
    // it): not the model's to answer, since the words are the writer's own; the part's rows
    // again, and nothing said.
    if (dictation_part(picked_intent_).first > 0) {
        status("picked as " + picked_intent_ + " out of its turn in the dictation, so nothing is said");
        picked_intent_.clear();
        offer();
        return;
    }

    // The turn number has to be the game's next one, not this app's. They diverge
    // whenever a turn was played that this app did not write, which happens on the
    // conversation after a restart and any time a write failed.
    const int turn = (played_ > written_ ? played_ : written_) + 1;

    // Words the game never heard go to the save on the reply's turn file, ahead of the
    // reply, so the mod remembers them in the order they were spoken. Only the words: the
    // direction the judge adds below is the app's, not the player's.
    if (remember && !said.empty()) pending_.push_back(said_directive(said));

    // Before the reply, so the command rides in the same turn file as the words that
    // acknowledge it, and so the reply can be told what just became true.
    std::string heard(said);
    const std::string judged = judge(said);
    if (!judged.empty()) heard += " " + judged;
    heard += direction_for(picked_intent_);
    // What they know of what was said, or the ask, on the same turn. Not when the judge has
    // just heard a yes: the words were an acceptance, and the direction for that is the
    // quest's. Only for words typed or spoken, or the one pick (`help`) that asks for the
    // favour: the other picks are the app's own rows, written with their answers.
    Advice advice;
    if (judged.empty() && (picked_intent_.empty() || picked_intent_ == "help")) {
        advice = advise(said, offering || picked_intent_ == "help");
        if (!advice.direction.empty()) {
            heard += " " + advice.direction;
            status("knowledge: " + advice.note);
        }
    }
    // A seed the model answers is spent like one answered from the database, so the list
    // after does not offer it again -- and so a giver's offer made in the model's own words
    // is followed by the accepting row, which written_fill lays once help has been asked.
    if (picked_intent_ == "help" || picked_intent_ == "about" || picked_intent_ == "ask") {
        if (std::find(told_.begin(), told_.end(), picked_intent_) == told_.end()) {
            told_.push_back(picked_intent_);
        }
    }
    // Spent, whichever way the judge went: the next words may be typed.
    picked_intent_.clear();

    // The reply reaches the game as it is generated, so the words appear on screen as the
    // model writes them rather than when it finishes. The transport is the same turn file
    // as ever, and it cannot be rewritten in place -- the engine caches a path once read --
    // so every update is a turn of its own carrying the whole text so far, flagged @more.
    // The flag tells the mod to put the new text where the old was rather than start a new
    // line, and to keep the player's keyboard shut. The last turn is the complete reply
    // with no flag, which is what hands the turn to the player. Updates go out no more
    // often than stream_every_, which is about as fast as the mod looks for them.
    //
    // The voice still gets the reply in speakable pieces, since a syllable is not
    // something it can start on.
    //
    // Recorded only once it has been written somewhere the game can read it, so a failed
    // write does not leave the model believing it said something the player never heard.
    // A write that fails part way stops the rest: the mod is left waiting on a reply that
    // will not finish, and F9 is the way back in for the player.
    int next = turn;
    bool wrote = false;
    bool failed = false;
    const auto ship = [&](const std::string& words) {
        if (failed) return;
        // Read for names as it goes out, so a person the model names mid-reply is in the
        // save by the time the reply is whole, and on the list after it.
        note_heard(words);
        if (!writer_.write(next, with_owed(words))) {
            status("turn " + std::to_string(next) + " could not be written");
            failed = true;
            return;
        }
        written_ = next++;
        owed_.clear();
        pending_.clear();
        wrote = true;
    };
    // The text so far as the screen should show it: no leading blank, since models like
    // to open with one, and nothing after the last visible character.
    const auto so_far = [&](const std::string& text) {
        const std::size_t from = text.find_first_not_of(" \t\r\n");
        if (from == std::string::npos) return std::string();
        const std::size_t to = text.find_last_not_of(" \t\r\n");
        return text.substr(from, to - from + 1);
    };

    // Small models like to open a reply by saying their last line again, word for word,
    // before going on -- qwen3:4b did it to every pick of "Is there anything I can help you
    // with?" straight after a greeting, on two backends. The copy is dropped off the front
    // and the reply is what follows it; while the text so far could still be the start of
    // such a copy, nothing is sent, so the screen never shows the line twice.
    const std::string previous =
        !history_.empty() && history_.back().role == "assistant" ? history_.back().content
                                                                 : std::string();

    SentenceBuffer buffer;
    Timing timing;
    std::string raw;                  // everything the model has written
    std::size_t fed = 0;              // how much of it the sentence buffer has been given
    std::optional<std::size_t> skip;  // where the reply proper starts in it, once known

    // What a copy could be: the old line whole, or its tail from a sentence on ("The wood's
    // still wet, and I've no time to spare." copied out of "Hm? What's it you want, sir? The
    // wood's still wet, and I've no time to spare."). Tails shorter than a couple of dozen
    // characters are not counted, since a reply may honestly open with the same "Aye.".
    std::vector<std::string_view> copies;
    if (!previous.empty()) {
        copies.emplace_back(previous);
        for (std::size_t at = 0; at < previous.size(); ++at) {
            if (previous[at] != '.' && previous[at] != '?' && previous[at] != '!') continue;
            const std::size_t next = previous.find_first_not_of(" \t\r\n", at + 1);
            if (next == std::string::npos || next == at + 1) continue;
            if (previous.size() - next >= 24) copies.emplace_back(previous.data() + next, previous.size() - next);
        }
    }
    // Decides, as soon as the words allow, whether the reply opens with one of them.
    const auto settle = [&]() {
        if (skip) return;
        if (copies.empty()) {
            skip = 0;
            return;
        }
        const std::size_t from = raw.find_first_not_of(" \t\r\n");
        if (from == std::string::npos) return;
        const std::string_view text(raw.data() + from, raw.size() - from);
        for (std::string_view copy : copies) {
            if (text.size() < copy.size() && copy.compare(0, text.size(), text) == 0) return;
        }
        // Longest first, and the whole line is the longest.
        for (std::string_view copy : copies) {
            if (text.compare(0, copy.size(), copy) == 0) {
                skip = from + copy.size();
                return;
            }
        }
        skip = 0;
    };
    // Gives the sentence buffer, and the voice, whatever has been settled and not yet given.
    //
    // Undashed on the way, so the screen, the voice and the history all have the same
    // words; a trailing hyphen or space waits for the next piece, unless this is the end.
    std::size_t clean_fed = 0;  // how much of the undashed reply has been given
    const auto feed = [&](bool whole = false) {
        if (!skip || raw.size() <= std::max(*skip, fed)) return;
        const std::string_view reply(std::string_view(raw).substr(*skip));
        const std::size_t safe = whole ? reply.size() : undash_safe(reply);
        if (*skip + safe <= fed) return;
        // Undashed from the start each time, since a dash reads by what is around it: up
        // to a safe point the result is a prefix of the whole's, so only the new tail goes.
        const std::string clean = undashed(reply.substr(0, safe));
        if (clean.size() > clean_fed) {
            for (std::string& ready : buffer.push(std::string_view(clean).substr(clean_fed))) {
                if (listener_.on_say) listener_.on_say(ready);
            }
            clean_fed = clean.size();
        }
        fed = *skip + safe;
    };

    // One try at the reply, streamed to the game as it settles. False means nothing was
    // written because the model could not be reached before a word had gone out.
    const auto generate = [&](const std::string& said_now, const Options& options) {
        // A reply guessed ahead for these very words, against this very history, is the
        // reply: it goes through the same settling as a streamed one, in one piece.
        for (auto guess = guesses_.begin(); guess != guesses_.end(); ++guess) {
            if (!guess->done || guess->heard != said_now) continue;
            if (!guess->text.empty()) {
                status("the reply to \"" + guess->text.substr(0, 40) + "\" was guessed ahead, and is said at once");
            }
            raw.append(guess->reply);
            settle();
            feed();
            timing = guess->timing;
            timing.guessed = true;
            guesses_.erase(guess);  // used, so not counted as wasted when the rows go
            ++guesses_used_;
            return true;
        }
        auto streamed_at = std::chrono::steady_clock::now();
        std::string streamed;
        try {
            timing = llm_.chat(conversation(world_, persona_, history_, said_now), options,
                               [&](std::string_view delta) {
                                   raw.append(delta);
                                   settle();
                                   feed();
                                   const auto now = std::chrono::steady_clock::now();
                                   if (now - streamed_at < stream_every_) return;
                                   const std::string text = so_far(buffer.text());
                                   if (text.empty() || text == streamed) return;
                                   streamed = text;
                                   streamed_at = now;
                                   ship("@more " + text);
                               });
        } catch (const std::exception& e) {
            status(std::string("the model could not be reached: ") + e.what());
            // Nothing reached the game, so nothing was said. Otherwise the reply ends where
            // it broke off, and the game is told so rather than left waiting for the rest.
            return wrote;
        }
        return true;
    };

    // True when the model wrote something, and all of it was the old line or the start of it.
    const auto copy_only = [&]() {
        if (raw.find_first_not_of(" \t\r\n") == std::string::npos) return false;
        return !skip || raw.find_first_not_of(" \t\r\n", *skip) == std::string::npos;
    };

    if (!generate(heard, options_)) return;
    if (copy_only()) {
        // None of it has reached the game or the voice. Asked once more, told plainly and a
        // little freer -- warmth alone did not shift qwen3:4b off a greeting it liked. If
        // that is a copy too it is said anyway, since the game is holding for a line. The
        // telling is not kept in the history: the reply stands as an answer to the words.
        status("the model only repeated its last line; asking again");
        raw.clear();
        skip.reset();
        fed = 0;
        clean_fed = 0;
        Options freer = options_;
        freer.temperature = std::min(1.5, freer.temperature + 0.3);
        const std::string told =
            heard + " [You have said your last line already. Do not say it again; answer them.]";
        if (!generate(told, freer)) return;
        if (copy_only()) {
            status("the model repeated its last line twice over, and it is said as it stands");
            skip = 0;
            feed();
        }
    } else if (skip && *skip > 0) {
        status("the reply opened by repeating the last line, which was dropped");
    }
    feed(true);
    if (std::optional<std::string> last = buffer.flush()) {
        if (listener_.on_say) listener_.on_say(*last);
    }

    const std::string reply = so_far(buffer.text());
    if (reply.empty()) {
        status("the model said nothing");
        return;
    }
    // Whole and unflagged, even when the last update already carried every word: this is
    // the turn that says the reply is over.
    ship(reply);
    if (!wrote || failed) return;

    // The direction goes into the history with the words, because it is part of what the
    // model answered and a later turn that lacked it would read the reply as unprompted.
    if (!heard.empty()) history_.push_back({"user", heard});
    history_.push_back({"assistant", reply});
    if (listener_.on_turn) listener_.on_turn(history_.back());
    if (listener_.on_timing) listener_.on_timing(timing);
    drew(advice.item_id, reply);
    // An ending the judge settled on a row the model answered: the rest of the
    // conversation is planned as after it.
    if (settling_) {
        Settled ending = std::move(*settling_);
        settling_.reset();
        adopt(std::move(ending));
    }

    // Now, while the player reads the line: the rows are wanted the moment they have.
    offer();
}

std::vector<MenuOption> Session::dictation_rows() const {
    std::vector<MenuOption> rows;
    const Quest& quest = persona_.quest_of;
    if (dictating_ < 1 || dictating_ > Quest::Dictation::kParts || !quest.has_dictation()) return rows;
    const Quest::Dictation::Part& part = quest.dictation.parts[dictating_ - 1];
    // As said, left out, softened, made larger: the order the writer chose them in, which is
    // the plain way first and the two that change the words after the one that drops them.
    for (const int outcome : part.outcomes()) {
        rows.push_back({static_cast<int>(rows.size()) + 1, dictation_intent(dictating_, outcome),
                        part.choice(outcome).row});
    }
    return rows;
}

std::vector<std::pair<std::string, std::string>> Session::alms_rows() const {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const char letter : alms_) {
        if (letter == 'm') rows.emplace_back("money", "Here's three groschen. Get yourself a hot meal.");
        else if (letter == 'f') rows.emplace_back("food", "Here, something to eat.");
        else if (letter == 'c') rows.emplace_back("clothes", "Take this; it'll keep the cold off better than what you've got.");
    }
    return rows;
}

std::optional<Session::WrittenAnswer> Session::written_answer(std::string_view intent) const {
    const Scripted& written = persona_.scripted;
    WrittenAnswer out;
    // Each written answer is said once a conversation. Picked again -- the offer, the account
    // of themselves -- the words are in the model's history, and it is left to find others.
    const auto once = [&](std::string_view key) {
        return std::find(told_.begin(), told_.end(), key) == told_.end();
    };
    if (intent == "help") {
        // The favour, while it is there to be asked: the giver's own words, and the row
        // that takes it up goes first on the next list, so the errand is taken on a pick
        // and nobody has to judge whether the model's "I'll see what I can do" was a yes.
        if (!written.offer.empty() && !persona_.quest.question.empty() && once("help")) {
            out.reply = written.offer;
            out.what = "the offer";
            if (!written.accept_row.empty()) out.next.push_back({0, "accept", written.accept_row});
            // And under it Henry's one question about the errand, while it is unasked, and
            // a letter's "what is in it?", which he may ask before he agrees to carry it.
            if (!written.more_row.empty() && once("more")) out.next.push_back({0, "more", written.more_row});
            if (!written.more_2_row.empty() && once("more_2")) out.next.push_back({0, "more_2", written.more_2_row});
            if (!written.pry_row.empty() && once("pry")) out.next.push_back({0, "pry", written.pry_row});
        } else if (!written.help.empty() && once("help")) {
            out.reply = written.help;
            out.what = "the help reply";
        }
        out.spent.emplace_back("help");
    } else if (intent == "accept") {
        // The errand taken up on its row, answered with the second telling where one was
        // written; the yes it owes the game rides with it (the judge is not asked either
        // way, see judge()). Without a second telling the model answers, as it always did.
        // A letter to be written at dictation begins here: the giver sits Henry down and
        // gives him the first part in the same breath, and the part's rows are the next list
        // (answer_scripted starts the dictation; offer lays them). The way to the receiver
        // (`agreed`) is not said here: nothing is to be carried yet, so it waits for the
        // sealing (notes.md 20 Oct 2026: no directions until the letter is written).
        const bool dictated = persona_.quest_of.has_dictation();
        if ((!written.accept_reply.empty() || dictated) && !persona_.quest.question.empty()) {
            if (dictated) {
                const Quest::Dictation& dictation = persona_.quest_of.dictation;
                out.reply = dictation.begin + "\n\n" + dictation.parts[0].dictate;
                out.what = "the dictation begun on his agreeing, with its first part";
                out.spent.emplace_back(kDictateIntent);
            } else {
                out.reply = written.accept_reply;
                out.what = persona_.quest_of.agreed.empty() ? "the second telling of the offer"
                                                            : "the written answer to his agreeing";
            }
            out.commands.push_back(persona_.quest.on_yes);
        }
        out.spent.emplace_back("accept");
    } else if (intent == kDictateIntent) {
        // The dictation picked up (or begun, after a yes the judge read): the giver's line
        // sitting Henry down, and the first part. From the start every time: the digits are
        // the mod's only once the letter is sealed, so a dictation left half done is begun
        // again (notes.md: Words Must Not Fail).
        const Quest& quest = persona_.quest_of;
        if (quest.has_dictation() && dictating_ == 0 && once(kDictateIntent)) {
            out.reply = quest.dictation.begin + "\n\n" + quest.dictation.parts[0].dictate;
            out.what = "the dictation begun, with its first part";
        }
        out.spent.emplace_back(kDictateIntent);
    } else if (const auto [part, outcome] = dictation_part(intent); part > 0) {
        // Henry's way with the part just dictated: the giver's written answer, then the next
        // part, or the sealing after the last, which owes the mod the letter as written
        // (kQuestWrote with the digits) and is the last word. Only for the part being chosen
        // on, and only a way written for it.
        const Quest& quest = persona_.quest_of;
        if (quest.has_dictation() && part == dictating_) {
            const Quest::Dictation& dictation = quest.dictation;
            const Quest::Dictation::Choice& choice = dictation.parts[part - 1].choice(outcome);
            if (choice.written()) {
                out.reply = choice.reply;
                if (part < Quest::Dictation::kParts) {
                    out.reply += "\n\n" + dictation.parts[part].dictate;
                    out.what = "the writer's answer to " + std::string(intent) + ", and the next part";
                } else {
                    // The sealing, and only now the way to the receiver (`agreed`): there is
                    // a letter to carry.
                    out.reply += "\n\n" + dictation.done;
                    if (!quest.agreed.empty()) out.reply += "\n\n" + quest.agreed;
                    out.what = "the writer's answer to " + std::string(intent) + ", and the sealing";
                    out.commands.push_back(wrote_command(wrote_ + static_cast<char>('0' + outcome)));
                }
            }
        }
    } else if (intent == "more" || intent == "more_2") {
        // Henry's question under the offer, answered as written. It settles nothing: the
        // accepting row is laid again, so the errand is still there to be taken, and the
        // other question with it while unasked.
        const bool second = intent == "more_2";
        const std::string& answer = second ? written.more_2_reply : written.more_reply;
        if (!answer.empty() && !persona_.quest.question.empty() && once(intent)) {
            out.reply = answer;
            out.what = "the written answer to his question about the errand";
            if (!written.accept_row.empty()) out.next.push_back({0, "accept", written.accept_row});
            if (!second && !written.more_2_row.empty() && once("more_2")) {
                out.next.push_back({0, "more_2", written.more_2_row});
            }
            if (second && !written.more_row.empty() && once("more")) {
                out.next.push_back({0, "more", written.more_row});
            }
        }
        out.spent.emplace_back(intent);
    } else if (intent == "remind") {
        if (!written.remind.empty() && once("remind")) {
            out.reply = written.remind;
            out.what = "the offer again, as a reminder";
        }
        out.spent.emplace_back("remind");
    } else if (intent == "pry") {
        // What the letter says, out of its writer: a Speech check the app weighs the game's
        // own way (DialogUtils.PersuadeByPlayer: Henry's stat over the writer's by a factor),
        // from the two stats the begin line carried. Either answer is written; the mod is
        // told the result so the game can show its stat-check mark. Unread stats fail it,
        // and the log says so, rather than giving the letter's matter away for nothing.
        if (!written.pry_row.empty() && once("pry")) {
            const bool read = approach_.speech >= 0 && approach_.npc_speech > 0;
            const bool passed = read && approach_.speech > approach_.npc_speech * written.pry_factor;
            out.reply = passed ? written.pry_yes : written.pry_no;
            out.what = std::string(passed ? "the letter's matter, told after a speech check won"
                                          : "a refusal, the speech check lost") +
                       " (speech " + std::to_string(approach_.speech) + " against " +
                       std::to_string(approach_.npc_speech) + (read ? "" : ", unread") + ")";
            out.directives.push_back(passed ? "@check speech pass" : "@check speech fail");
            // Asked before agreeing, it settles nothing: the accepting row is laid again.
            if (!persona_.quest.question.empty() && !written.accept_row.empty()) {
                out.next.push_back({0, "accept", written.accept_row});
            }
        }
        out.spent.emplace_back("pry");
    } else if (intent == "about") {
        if (!written.about.empty() && once("about")) {
            out.reply = written.about;
            out.what = "the self-account";
        }
        out.spent.emplace_back("about");
    } else if (intent == "ask") {
        // Three of what they know and have not yet said, as a question back, with a row
        // for each on the next list. Asked again with nothing picked, the next three.
        std::vector<Topic> open;
        for (const Topic& topic : written.topics) {
            if (knows_of(topic) && once(topic.key)) open.push_back(topic);
        }
        std::vector<Topic> chosen;
        if (!open.empty()) {
            // Three or fewer left go in their order; more than three turn with the rounds.
            const std::size_t start =
                open.size() > 3 ? static_cast<std::size_t>(ask_rounds_) * 3 % open.size() : 0;
            for (std::size_t i = 0; i < open.size() && chosen.size() < 3; ++i) {
                chosen.push_back(open[(start + i) % open.size()]);
            }
        }
        if (!chosen.empty()) {
            // The opener varies by soul, so two neighbours do not both say "Depends what
            // about"; and by round, so the same soul asked twice does not either.
            unsigned opener = static_cast<unsigned>(ask_rounds_);
            for (const char c : persona_.name) opener = opener * 31u + static_cast<unsigned char>(c);
            out.reply = ask_reply(chosen, opener);
            out.what = std::to_string(chosen.size()) + " topics to choose from";
            for (const Topic& topic : chosen) out.next.push_back({0, ask_intent(topic.key), ask_row(topic)});
            out.ask_round = true;
        }
    } else if (intent == "alms") {
        // Henry has something for a beggar: the rows for what the mod says he could spare,
        // one each until given. The words are the app's -- no beggar has a line written for
        // being given to -- and the second asking is shorter than the first.
        for (const auto& [kind, row] : alms_rows()) {
            if (once(alms_intent(kind))) out.next.push_back({0, alms_intent(kind), row});
        }
        if (!out.next.empty()) {
            out.reply = once("alms") ? "God bless you, sir. Whatever you can spare, I'll not refuse."
                                     : "Whatever it is, sir, I'll take it gladly.";
            out.what = "the beggar's thanks, with " + std::to_string(out.next.size()) + " things to give";
        }
        out.spent.emplace_back("alms");
    } else if (const std::string_view kind = alms_kind(intent); !kind.empty()) {
        // The thing given: the mod moves it on @alms (and for a garment dresses him in it),
        // and the thanks are said as written here. Once a kind a conversation.
        bool offered = false;
        for (const auto& [have, row] : alms_rows()) offered = offered || have == kind;
        if (once(intent) && offered) {
            if (kind == "money") out.reply = "Three groschen! God reward you, sir. That's bread for days.";
            else if (kind == "food") out.reply = "Bless you. I'll not ask what it is; I'll eat it.";
            else out.reply = "Oh... bless you, sir. Let me put it on, before someone takes it off me.";
            out.what = "the thanks for " + std::string(kind);
            out.directives.push_back("@alms " + std::string(kind));
        }
        out.spent.emplace_back(intent);
    } else if (const std::string_view key = ask_key(intent); !key.empty()) {
        for (const Topic& topic : written.topics) {
            if (topic.key != key) continue;
            out.reply = topic.text;
            out.what = "what they know of " + topic.label;
            out.spent.emplace_back(key);
            break;
        }
    } else if (const std::string_view remark_key = say_key(intent); !remark_key.empty()) {
        // A remark of Henry's the generator wrote for this soul, answered as written.
        for (const Remark& remark : written.remarks) {
            if (remark.key != remark_key) continue;
            out.reply = remark.text;
            out.what = "the written answer to that remark";
            out.spent.emplace_back(intent);
            break;
        }
    } else if (intent == "apologise") {
        // The holder's condition put to the giver. His answer was decided with the plan and
        // is written; what it owes the game -- his word given or withheld -- rides with it.
        const QuestPlan& plan = persona_.quest;
        if (!plan.apologise_reply.empty() && once("apologise")) {
            out.reply = plan.apologise_reply;
            out.what = plan.apologise_command == kQuestApologise ? "the giver giving his word"
                                                                 : "the giver refusing";
            out.commands.push_back(plan.apologise_command);
        }
        out.spent.emplace_back("apologise");
    } else {
        // A row the director wrote (a deliver's report or keep): the reply and what it owes
        // the game were decided with the plan. Once each; and once one of them is said the
        // errand is over, so the other is taken as told too.
        for (const DirectorRow& row : persona_.quest.rows) {
            if (row.intent != intent || !once(intent)) continue;
            // A row with a check to carry off (a letter's "it is delivered" before it is):
            // weighed as the pry is, from the two Speech stats on the begin line. Lost, the
            // soul's written catch, nothing owed, this row alone spent; the mod shows the mark.
            if (!row.check.empty()) {
                const bool read = approach_.speech >= 0 && approach_.npc_speech > 0;
                const bool passed = read && approach_.speech > approach_.npc_speech * row.check_factor;
                out.directives.push_back(passed ? "@check speech pass" : "@check speech fail");
                if (!passed) {
                    out.reply = row.fail_reply;
                    out.what = "the writer catching the " + std::string(intent) + " (speech " +
                               std::to_string(approach_.speech) + " against " + std::to_string(approach_.npc_speech) +
                               (read ? "" : ", unread") + ")";
                    out.spent.emplace_back(intent);
                    break;
                }
            }
            out.reply = row.reply;
            out.what = "the writer's answer to " + std::string(intent) +
                       (row.check.empty() ? "" : ", the speech check won");
            if (!row.command.empty()) out.commands.push_back(row.command);
            // The chest taken up again settles nothing, and a grouped row (the giver's
            // carrying and its refusal) spends its group alone; every other row of the
            // director's ends the errand, so the rest are taken as told too.
            if (!row.group.empty()) {
                for (const DirectorRow& other : persona_.quest.rows) {
                    if (other.group == row.group) out.spent.emplace_back(other.intent);
                }
            } else if (row.command != kQuestNamed) {
                for (const DirectorRow& other : persona_.quest.rows) out.spent.emplace_back(other.intent);
                // And the reminder: an errand just settled is not one to be reminded of, and the
                // row stood on the list after Anna had been told where her cat was (notes.md,
                // 8 Oct 2026).
                out.spent.emplace_back("remind");
            } else {
                out.spent.emplace_back(intent);
            }
            break;
        }
    }
    if (out.reply.empty()) return std::nullopt;
    return out;
}

bool Session::answer_scripted(std::string_view said, bool remember) {
    const std::string intent = picked_intent_;
    std::optional<WrittenAnswer> found = written_answer(intent);
    if (!found) return false;
    for (const std::string& command : found->commands) owe(command);
    for (const std::string& directive : found->directives) pending_.push_back(directive);
    for (const std::string& key : found->spent) told_.push_back(key);
    if (found->ask_round) ++ask_rounds_;
    // A row whose reply is the last word (DirectorRow::closes; the carrying declined).
    for (const DirectorRow& row : persona_.quest.rows) {
        if (row.intent == intent && row.closes) closing_ = true;
    }
    // A dictated letter: begun on the accepting row or the row that picks it up, and a part
    // on from each way chosen with it; the last part's choice ends it (the command owed
    // above closes the conversation on the sealing).
    if (persona_.quest_of.has_dictation()) {
        if (intent == "accept" || intent == kDictateIntent) {
            dictating_ = 1;
            wrote_.clear();
            dictation_due_ = false;
        } else if (const auto [part, outcome] = dictation_part(intent); part > 0 && part == dictating_) {
            wrote_ += static_cast<char>('0' + outcome);
            dictating_ = part < Quest::Dictation::kParts ? part + 1 : 0;
        }
    }

    status("picked as " + intent + ", answered with " + found->what + " as written");
    // The accepting row settles the errand as judge() would have: the yes is owed above,
    // the question is answered for the conversation, and the model's history carries the
    // direction with the words so a later turn knows what was agreed.
    std::string heard;
    if (intent == "accept") {
        heard = std::string(said) + " " + persona_.quest.direction;
        persona_.quest.question.clear();
    }
    // A row of the director's that ends the errand: the record is kept and the conversation
    // planned again on the ending before the reply is written, so the command owed goes out
    // on the errand that ended; what follows is adopted after. Where another errand of this
    // giver's follows, its offer is said with the thanks -- one reply in two beats -- and
    // the accepting row goes first on the next list, so there is no second "Is there
    // anything I can help with?" to miss (notes.md, 8 Oct 2026).
    std::optional<Settled> ending;
    std::string reply = found->reply;
    for (const DirectorRow& row : persona_.quest.rows) {
        if (row.intent != intent) continue;
        // Only a row whose command went out: a lie the writer caught (the speech check
        // lost) owed nothing and ended nothing, and the errand stands to be lied about
        // again or given up.
        const bool owed = std::find(found->commands.begin(), found->commands.end(), row.command) !=
                          found->commands.end();
        if (owed && (row.command == kQuestComplete || row.command == kQuestLie || row.command == kQuestQuiet ||
                     row.command == kQuestConfess || row.command == kQuestKeep || row.command == kQuestAbandon)) {
            ending = settle(row.command);
            if (ending->leads_on) {
                const Scripted& coming = ending->next.scripted;
                // As on the greeting: the thanks written for leading on, where there are some.
                if (row.command == kQuestComplete && !persona_.quest_of.thanks_onward.empty()) {
                    reply = persona_.quest_of.thanks_onward;
                }
                reply += "\n\n" + coming.offer;
                found->next.clear();
                if (!coming.accept_row.empty()) found->next.push_back({0, "accept", coming.accept_row});
                if (!coming.more_row.empty()) found->next.push_back({0, "more", coming.more_row});
                if (!coming.more_2_row.empty()) found->next.push_back({0, "more_2", coming.more_2_row});
                told_.emplace_back("help");
            }
        }
        break;
    }
    // An errand ended with nothing to lead on to closes the conversation on the giver's
    // answer, as taking one up does (notes.md 23 Oct 2026: Christine thanked Henry for the
    // prayer and the menu stood there). Whatever else they have to say is a fresh talk.
    if (ending && ending->ended && !ending->leads_on) closing_ = true;
    if (!say_written(said, reply, remember, heard)) return true;
    // The offer said is the ask put, for a later line that offers help again.
    if (found->what == "the offer" && !drew_.empty()) {
        drew_.back().item_id = "quest:" + persona_.quest_of.id;
    }
    if (ending) {
        const bool leads_on = ending->leads_on;
        adopt(std::move(*ending));
        if (leads_on && !drew_.empty()) drew_.back().item_id = "quest:" + persona_.quest_of.id;
    }
    preset_ = std::move(found->next);
    offer();
    return true;
}

Session::Settled Session::settle(std::string_view command) {
    Settled settled;
    const Quest ended = persona_.quest_of;
    if (!ended.exists() || ended.role != QuestRole::Giver) return settled;
    settled.ended = true;
    // The record as the mod will dump it next time: the errand at its furthest stage, and
    // how it ended as the store writes it (emit.cpp's hand-in: lie is `lied`, keep `kept`,
    // quiet `quiet`, confess `dead`; a plain complete keeps the how the holder's leg set,
    // which the dump already carries).
    const std::string how = command == kQuestLie ? "lied" : command == kQuestKeep ? "kept"
                            : command == kQuestQuiet ? "quiet" : command == kQuestConfess ? "dead"
                            : command == kQuestAbandon ? "abandoned"
                            : std::string();
    const auto put = [](std::vector<std::pair<std::string, std::string>>& list, const std::string& id,
                        const std::string& value) {
        for (auto& [key, held] : list) {
            if (key == id) {
                held = value;
                return;
            }
        }
        list.emplace_back(id, value);
    };
    if (record_.soul.empty()) {
        // No dump came (a mod too old to keep one, or a first meeting unrecorded): a record
        // of this much, whole as far as it goes.
        record_.soul = soul_;
        record_.complete = true;
    }
    put(record_.quests, ended.id, "done");
    if (!how.empty()) put(record_.hows, ended.id, how);
    if (record_.quest == ended.id) record_.facts.done = true;

    settled.next = persona_for(soul_, approach_, anim_);
    const Persona& next = settled.next;
    const Quest& after = next.quest_of;
    // What is adopted is the plan and the sheet; the greeting was said and the history
    // stands. The sheet changing costs the cache once, which a story's turning point is
    // worth. Taken only when the new plan is plainly the ending's: the same errand planned
    // as done (no rows left to settle it), or another of this giver's with its offer ready
    // -- a plan that still has the hand-in's rows is the save not having followed, and is
    // left alone rather than laid twice.
    const bool same_done = after.id == ended.id && next.quest.rows.empty() && next.quest.question.empty();
    settled.leads_on = after.exists() && after.id != ended.id && after.role == QuestRole::Giver &&
                       !next.scripted.offer.empty() && !next.quest.question.empty();
    settled.adopt = same_done || settled.leads_on;
    return settled;
}

void Session::adopt(Settled settled) {
    if (!settled.adopt) {
        persona_.scripted.remind.clear();
        return;
    }
    Persona& next = settled.next;
    persona_.sheet = std::move(next.sheet);
    persona_.parts = std::move(next.parts);
    persona_.quest = std::move(next.quest);
    persona_.quest.opening_commands.clear();
    persona_.quest.greeting.clear();
    persona_.quest_of = std::move(next.quest_of);
    persona_.scripted = std::move(next.scripted);
    persona_.scripted.remind.clear();
    if (settled.leads_on) {
        status("the errand is settled, and " + persona_.name + " has " +
               (persona_.quest_of.name.empty() ? "another errand" : persona_.quest_of.name) + " (" +
               persona_.quest_of.id + ") to ask next: its offer is said with the thanks");
    } else {
        status("the errand is settled, and the rest of the conversation is planned as after it");
    }
}

bool Session::say_written(std::string_view said, const std::string& reply, bool remember,
                          std::string_view heard) {
    // The player's words to the save first, as answer() sends them, then the reply as one
    // whole turn: nothing is being generated, so there is nothing to stream.
    if (remember && !said.empty()) pending_.push_back(said_directive(said));
    note_heard(reply);
    const int turn = next_turn();
    const int last = write_beats(turn, reply);
    if (last == 0) {
        status("turn " + std::to_string(turn) + " could not be written");
        return false;
    }
    written_ = last;
    owed_.clear();
    pending_.clear();

    // The voice takes it in the same pieces a generated reply arrives in.
    SentenceBuffer buffer;
    for (std::string& piece : buffer.push(reply)) {
        if (listener_.on_say) listener_.on_say(piece);
    }
    if (const std::optional<std::string> last = buffer.flush()) {
        if (listener_.on_say) listener_.on_say(*last);
    }
    // In the history as though generated, since the model has to go on as the person who
    // said it; the words are the whole of the turn unless a direction was settled with them.
    if (!heard.empty()) history_.push_back({"user", std::string(heard)});
    else if (!said.empty()) history_.push_back({"user", std::string(said)});
    history_.push_back({"assistant", reply});
    if (listener_.on_turn) listener_.on_turn(history_.back());
    drew({}, reply);
    return true;
}

std::vector<MenuOption> Session::written_fill(bool first_round, int room) {
    std::vector<MenuOption> fill;
    if (room <= 0) return fill;
    const Scripted& written = persona_.scripted;
    const auto spent = [&](std::string_view key) {
        return std::find(told_.begin(), told_.end(), key) != told_.end();
    };
    const auto on_list = [&](std::string_view intent) {
        for (const MenuOption& row : rows_) {
            if (row.intent == intent) return true;
        }
        for (const MenuOption& row : fill) {
            if (row.intent == intent) return true;
        }
        return false;
    };
    const auto take = [&](std::string intent, std::string text) {
        if (static_cast<int>(fill.size()) >= room || on_list(intent)) return;
        fill.push_back({0, std::move(intent), std::move(text)});
    };

    // The row that takes the favour up stays on the list from the offer until it is picked
    // or the errand is taken some other way, so a player who asked something else first
    // has not lost it. (The round straight after a written offer has it first, laid by the
    // answer; this is for the rounds after that, and for the round after an offer the
    // model made in its own words.)
    if (!written.accept_row.empty() && spent("help") && !persona_.quest.question.empty()) {
        take("accept", written.accept_row);
        // His question about it stays under the accepting row until asked, and so does a
        // letter's "what is in it?".
        if (!written.more_row.empty() && !spent("more")) take("more", written.more_row);
        if (!written.more_2_row.empty() && !spent("more_2")) take("more_2", written.more_2_row);
        if (!written.pry_row.empty() && !spent("pry")) take("pry", written.pry_row);
    }
    // At a beggar, the row that gives, on every round while something is left to give: a
    // man begging is why the player stopped, and the gift should not wait on a second list.
    bool giving = false;
    for (const MenuOption& row : rows_) giving = giving || !alms_kind(row.intent).empty();
    for (const auto& [kind, row] : alms_rows()) {
        if (!giving && !spent(alms_intent(kind))) {
            take("alms", std::string(kAlmsRow));
            break;
        }
    }
    // The offer of help and the account of themselves are the opening round's seeds, and
    // stay on offer until picked while there is a written answer to them: the offer is the
    // reason the soul is worth talking to, and a player who took another row first should
    // not lose it. Not on the opening round, where the seeds have already gone.
    if (!first_round) {
        if (!written.remind.empty()) {
            if (!spent("remind")) take("remind", std::string(kRemindRow));
        } else if (written.help_due && !spent("help")) {
            take("help", help_row());
        }
        if (!written.about.empty() && !spent("about")) take("about", std::string(kFirstRoundSeeds[2]));
    }

    // Then what they know and the remarks written for them, a row each, interleaved so a
    // list is not all questions or all small talk, and turned with the rounds so the rows
    // a player passed over are not the same rows every time.
    std::vector<MenuOption> pool;
    std::size_t t = 0, r = 0;
    while (t < written.topics.size() || r < written.remarks.size()) {
        if (t < written.topics.size()) {
            const Topic& topic = written.topics[t++];
            if (!spent(topic.key) && knows_of(topic)) pool.push_back({0, ask_intent(topic.key), ask_row(topic)});
        }
        if (r < written.remarks.size()) {
            const Remark& remark = written.remarks[r++];
            const std::string intent = say_intent(remark.key);
            if (!spent(intent)) pool.push_back({0, intent, remark.row});
        }
    }
    if (!pool.empty()) {
        const std::size_t start = static_cast<std::size_t>(fill_offset_) % pool.size();
        const std::size_t before = fill.size();
        for (std::size_t i = 0; i < pool.size() && static_cast<int>(fill.size()) < room; ++i) {
            const MenuOption& row = pool[(start + i) % pool.size()];
            take(row.intent, row.text);
        }
        fill_offset_ += static_cast<int>(fill.size() - before);
    }
    return fill;
}

void Session::offer() {
    rows_.clear();
    drop_guesses();
    if (!menu_ || !talking_) return;
    // Taken up, the conversation is closing on the mod's side; a menu would be drawn over
    // the leaving.
    if (closing_) {
        status("the errand is taken, so the conversation closes and no rows are offered");
        return;
    }
    // The opening round is the one where the NPC has spoken and Henry has not, however the
    // opening line came about; it is prompted from the design's three, the rest are free.
    const bool first_round = history_.size() == 1;

    // Every row is written: the design's seeds, the director's, and what the database has
    // for this soul. The model is not asked to fill a list that comes up short -- a row it
    // wrote would be a row it had to answer, and the player leaves (or types) when the list
    // is empty. Nothing on the list waits on the model, so the whole round goes to the game
    // on one turn file at the end, with its count, and the mod draws it as it stands: a
    // list of three is three rows and the way out, not three and a dimmed fourth.
    const auto add = [&](std::vector<MenuOption> options) {
        for (MenuOption& option : options) {
            option.row = static_cast<int>(rows_.size()) + 1;
            if (listener_.on_option) listener_.on_option(option);
            rows_.push_back(std::move(option));
        }
    };
    const auto send = [&](bool closed = false) {
        const int turn = next_turn();
        if (!writer_.write(turn, with_owed(whole_list_turn(rows_, closed)))) {
            status("turn " + std::to_string(turn) + " could not be written");
            return;
        }
        written_ = turn;
        owed_.clear();
        pending_.clear();
    };

    // A letter being written at dictation: the part just given is the whole of the round,
    // Henry's ways with it and nothing else, so the choice is made with the words in his ear
    // and the list cannot crowd a way off. Closed: the mod leaves its say row and its way
    // out off it (notes.md 20 Oct 2026) -- a letter half dictated is not a conversation to
    // walk out of, and a line of Henry's own is not one of the ways with a part. (A player
    // who leaves anyway, by the game's own means, gets greet_unwritten and starts over.)
    if (dictating_ > 0) {
        preset_.clear();
        add(dictation_rows());
        send(/*closed=*/true);
        status("offered the dictation's " + std::to_string(rows_.size()) + " ways with part " +
               std::to_string(dictating_));
        return;
    }
    // The opening round's three go at once: the player has a menu the moment the NPC stops
    // speaking. So do the rows the last answer decided -- the accepting line after a written
    // offer, the topics after "I have a question".
    std::vector<MenuOption> preset = std::move(preset_);
    preset_.clear();
    if (static_cast<int>(preset.size()) > kMenuOptions) preset.resize(kMenuOptions);
    // The dictation due and not begun (the errand taken up on a yes the judge read): the row
    // that picks it up goes first, unless the plan laid it already.
    if (dictation_due_ && std::find(told_.begin(), told_.end(), kDictateIntent) == told_.end() &&
        std::none_of(persona_.quest.rows.begin(), persona_.quest.rows.end(),
                     [](const DirectorRow& row) { return row.intent == kDictateIntent; })) {
        const Quest::Dictation& dictation = persona_.quest_of.dictation;
        add({{0, std::string(kDictateIntent),
              dictation.resume.empty() ? std::string(kDictateRow) : dictation.resume}});
    }
    if (first_round) {
        // Rows laid for the opening round go first: the accepting row of an offer the
        // greeting itself made (an errand paid off on sight that leads on to the giver's
        // next), with the offer of help already spent on it.
        if (!preset.empty()) {
            add(preset);
            preset.clear();
        }
    }
    // The errand's own rows next, on every list until picked, and ahead of everything
    // else: they are what the player came back for, and "I gave the letter to Father
    // Ernest" under "Tell me about yourself" was the wrong order (notes.md 9 Oct 2026).
    // The director's rows (a deliver's report and keep, a travel's three), then the
    // holder's condition a dispute carries, then a letter's "what is in it?" once the
    // errand is taken up (before that it is laid with the offer).
    // The give-up row is the one exception: on every list while the errand is open, but
    // last on it, with a place kept for it (notes.md 13 Oct 2026).
    const DirectorRow* give_up = nullptr;
    for (const DirectorRow& row : persona_.quest.rows) {
        if (std::find(told_.begin(), told_.end(), row.intent) != told_.end()) continue;
        // A row for after the yes (the giver's carrying) waits for it: on the list once the
        // question is answered, and only then.
        if (row.after_yes && !persona_.quest.question.empty()) continue;
        if (row.intent == "give_up") {
            give_up = &row;
            continue;
        }
        if (static_cast<int>(rows_.size()) >= kMenuOptions - (give_up ? 1 : 0)) break;
        add({{0, row.intent, row.text}});
    }
    const int room = kMenuOptions - (give_up ? 1 : 0);
    if (!persona_.quest.apologise_row.empty() &&
        std::find(told_.begin(), told_.end(), "apologise") == told_.end() &&
        static_cast<int>(rows_.size()) < room) {
        add({{0, "apologise", persona_.quest.apologise_row}});
    }
    if (!persona_.scripted.pry_row.empty() && persona_.quest.question.empty() &&
        std::find(told_.begin(), told_.end(), "pry") == told_.end() &&
        static_cast<int>(rows_.size()) < room) {
        add({{0, "pry", persona_.scripted.pry_row}});
    }
    if (first_round) {
        // On a list the errand's rows would crowd off, the seeds give way from the end,
        // down to the offer of help alone.
        std::vector<MenuOption> seeds = first_round_rows();
        // The offer of help is for a soul with an errand to give, or a lead to one
        // (Scripted::help_due); on everyone else it is not asked, since every soul in town
        // being asked what Henry can do for them reads as a questionnaire, and the answer
        // would be the model's.
        if ((!persona_.scripted.help_due && persona_.scripted.remind.empty()) ||
            std::find(told_.begin(), told_.end(), "help") != told_.end()) {
            seeds.erase(std::remove_if(seeds.begin(), seeds.end(),
                                       [](const MenuOption& row) { return row.intent == "help"; }),
                        seeds.end());
        }
        // At a giver whose errand Henry has taken up and not finished, the offer of help
        // is the wrong question; the row asks to be reminded instead, and is answered with
        // the offer again. Dropped altogether once the reminder has been given this visit.
        for (MenuOption& row : seeds) {
            if (row.intent == "help" && persona_.scripted.remind.empty()) row.text = help_row();
        }
        if (!persona_.scripted.remind.empty()) {
            const bool reminded = std::find(told_.begin(), told_.end(), "remind") != told_.end();
            for (MenuOption& row : seeds) {
                if (row.intent != "help") continue;
                row.intent = "remind";
                row.text = std::string(kRemindRow);
            }
            if (reminded) {
                seeds.erase(std::remove_if(seeds.begin(), seeds.end(),
                                           [](const MenuOption& row) { return row.intent == "remind"; }),
                            seeds.end());
            }
        }
        // A question from somebody this soul does not yet trust with anything they know
        // has nothing written behind it, so it is not offered; the row comes back when a
        // favour or the meetings have earned it. The same when all they would tell is about
        // people Henry has not heard of yet: the question comes back once one is named.
        if (nothing_to_ask()) {
            seeds.erase(std::remove_if(seeds.begin(), seeds.end(),
                                       [](const MenuOption& row) { return row.intent == "ask"; }),
                        seeds.end());
        }
        const std::size_t keep = static_cast<std::size_t>(
            std::max(1, room - static_cast<int>(rows_.size())));
        if (seeds.size() > keep) seeds.resize(keep);
        add(std::move(seeds));
    }
    if (!preset.empty()) add(preset);
    // Whatever else is written and not yet spent fills the list, since a written row is
    // answered without the model too, and a round that is whole at once is what makes the
    // menu feel like the game's own. A list that comes up short stays short.
    if (std::vector<MenuOption> fill = written_fill(first_round, room - static_cast<int>(rows_.size()));
        !fill.empty()) {
        add(std::move(fill));
    }
    if (give_up != nullptr && static_cast<int>(rows_.size()) < kMenuOptions) add({{0, give_up->intent, give_up->text}});
    // Sent even empty: the mod then draws the way out alone, at once, rather than waiting
    // its grace for rows that are not coming.
    send();
    if (rows_.empty()) {
        status("no rows to offer, so the list is the way out alone");
        return;
    }
    status("offered " + std::to_string(rows_.size()) + " rows, all written");
    plan_guesses();
}

std::string Session::direction_for(std::string_view intent) const {
    const bool about_said = std::find(told_.begin(), told_.end(), "about") != told_.end();
    if (intent == "about" && (persona_.scripted.about_told || about_said)) {
        return std::string(kAboutAgain);
    }
    // The director's rows the model answers when nothing was written for them.
    if (intent == "give_up") {
        return " [They have just told you they are giving up the errand you asked of them; they "
               "will not do it. It is over between you from here. Answer as you would, in a line or "
               "two, and ask nothing more of them.]";
    }
    if (intent == "accept_again") {
        return " [They have just agreed to take it up again the way you have put it to them. "
               "Answer as you would, in a line or two, and let them go to it.]";
    }
    return {};
}

std::optional<std::string> Session::heard_for(const MenuOption& row) const {
    std::string heard = row.text;
    // What judge() would do with the pick: an accepting row is the yes and carries the
    // direction without asking; agreement and help after the offer are put to the judge,
    // whose verdict decides the direction, so those cannot be guessed.
    if (!persona_.quest.question.empty()) {
        if (row.intent == "accept") {
            heard += " " + persona_.quest.direction;
        } else if ((row.intent == "agree" || row.intent == "help") &&
                   row.text != kFirstRoundSeeds[0] && row.text != help_row()) {
            return std::nullopt;
        }
    }
    heard += direction_for(row.intent);
    return heard;
}

void Session::plan_guesses() {
    drop_guesses();
    if (!speculate_ || !talking_) return;
    for (const MenuOption& row : rows_) {
        if (written_answer(row.intent)) continue;
        const std::optional<std::string> heard = heard_for(row);
        if (!heard) continue;
        guesses_.push_back({row.intent, row.text, *heard, {}, {}, false});
    }
}

void Session::drop_guesses() {
    for (const Guess& guess : guesses_) {
        if (guess.done) ++guesses_wasted_;
    }
    guesses_.clear();
}

void Session::keep_greeting(GreetingGuess&& guess) {
    // One per soul and sheet: the newer stands in for the older, which is a repeat.
    for (auto it = kept_greetings_.begin(); it != kept_greetings_.end(); ++it) {
        if (it->soul == guess.soul && it->persona.sheet == guess.persona.sheet) {
            kept_greetings_.erase(it);
            break;
        }
    }
    while (kept_greetings_.size() >= kKeptGreetings) {
        // Nobody came back for it, which is the one way a kept line is wasted.
        ++guesses_wasted_;
        kept_greetings_.erase(kept_greetings_.begin());
    }
    kept_greetings_.push_back(std::move(guess));
}

std::optional<Session::GreetingGuess> Session::take_kept_greeting(std::string_view soul,
                                                                  std::string_view sheet) {
    for (auto it = kept_greetings_.begin(); it != kept_greetings_.end(); ++it) {
        if (it->soul == soul && it->persona.sheet == sheet) {
            GreetingGuess found = std::move(*it);
            kept_greetings_.erase(it);
            return found;
        }
    }
    return std::nullopt;
}

bool Session::look_held() const {
    return std::chrono::steady_clock::now() >= look_due_;
}

bool Session::has_speculation() const {
    if (!talking_) {
        return look_prepare_.has_value() || (greeting_guess_ && !greeting_guess_->done) ||
               fold_.has_value();
    }
    // Not while the microphone is open: the transcriber runs on the same cores, and a
    // guess at a row the player has left the list to speak is the least likely one to be
    // used anyway.
    if (listening_) return false;
    // The open soul's sheet, when the press came before a look had read it in (greet).
    if (!prefiller_.is_warm(persona_)) return true;
    for (const Guess& guess : guesses_) {
        if (!guess.done) return true;
    }
    return false;
}

std::optional<bool> Session::look_ahead(const std::function<bool()>& interrupted) {
    // The voice first, on the written greeting's beats, so that the audio is ready at
    // the press; the poller holds the model's guess below while the voice is at work.
    if (look_prepare_) {
        PendingPrepare prepare = std::move(*look_prepare_);
        look_prepare_.reset();
        if (listener_.on_prepare) {
            for (const std::string& beat : prepare.beats) listener_.on_prepare(beat, prepare.actor);
        }
        status("the look at " + looked_soul_ + " has lasted, so their greeting is being made ahead");
        if (!prepare.beats.empty() && greeting_guess_ && !greeting_guess_->done) return true;
    }
    // Between conversations the only guess is the opening line of whoever is being
    // looked at: the very call open() would make for them, an empty history and no
    // words of the player's, so the cache it leaves is the one that call would want.
    if (!greeting_guess_ || greeting_guess_->done) return std::nullopt;
    GreetingGuess& guess = *greeting_guess_;
    ++guess.runs;
    if (guess.warm_only) {
        if (!prefiller_.warm_persona(world_, guess.persona, interrupted)) {
            ++guess.stops;
            return true;
        }
        guess.done = true;
        status(guess.persona.name + "'s sheet is read in");
        return false;
    }
    std::string raw;
    try {
        const Timing timing = llm_.chat(conversation(world_, guess.persona, {}, ""), options_,
                                        [&](std::string_view delta) {
                                            raw.append(delta);
                                            if (interrupted()) throw Interrupted{};
                                        });
        guess.reply = std::move(raw);
        guess.timing = timing;
        guess.done = true;
        prefiller_.mark_warm(guess.persona);
        status("guessed " + guess.persona.name + "'s opening line ahead, " +
               std::to_string(timing.reply_tokens) + " tokens");
    } catch (const Interrupted&) {
        // Picked up again next time round, unless what interrupted it was a look at
        // somebody else or a begin, which replace or spend it.
        ++guess.stops;
        return true;
    } catch (const std::exception& e) {
        status(std::string("the model could not be reached for a guess: ") + e.what());
        greeting_guess_.reset();
        return false;
    }
    return false;
}

bool Session::speculate(const std::function<bool()>& interrupted) {
    // The sheet first, in a conversation: every reply prefills it otherwise, and the
    // guesses below would each read it in on their own. Given up between pieces when a
    // line arrives, and taken up again next time round, from where the cache got to.
    if (talking_ && !prefiller_.is_warm(persona_)) {
        if (!prefiller_.warm_persona(world_, persona_, interrupted)) return true;
        status(persona_.name + "'s sheet is read in, with the greeting out");
        return has_speculation();
    }
    if (!talking_) {
        // The soul being looked at comes first: their line is the one the player is about
        // to want. A fold due from the last conversation waits behind it, and is what the
        // model does when nobody is being looked at.
        if (look_held()) {
            if (const std::optional<bool> more = look_ahead(interrupted)) return *more;
        }
        return fold(interrupted);
    }
    Guess* next = nullptr;
    for (Guess& guess : guesses_) {
        if (!guess.done) {
            next = &guess;
            break;
        }
    }
    if (!next) return false;
    // The very call answer() would make, so the prefix is the one already cached and the
    // words are the ones the settling expects; only nothing is shipped as it comes.
    std::string raw;
    try {
        const Timing timing = llm_.chat(conversation(world_, persona_, history_, next->heard), options_,
                                        [&](std::string_view delta) {
                                            raw.append(delta);
                                            if (interrupted()) throw Interrupted{};
                                        });
        next->reply = std::move(raw);
        next->timing = timing;
        next->done = true;
        status("guessed the reply to \"" + next->text.substr(0, 40) + "\" ahead, " +
               std::to_string(timing.reply_tokens) + " tokens");
    } catch (const Interrupted&) {
        // Left undone: whatever interrupted it may have changed the history, in which case
        // the rows go and the guess with them; if not, it is picked up again next time.
        return true;
    } catch (const std::exception& e) {
        status(std::string("the model could not be reached for a guess: ") + e.what());
        guesses_.clear();
        return false;
    }
    return has_speculation();
}

void Session::resume(int played) {
    if (played <= 0) return;
    if (played > played_) played_ = played;
    if (played > written_) written_ = played;
    status("joining a game that has played " + std::to_string(played) + " turns already; the next is " +
           std::to_string(next_turn()));
}

void Session::resume_hooks(int hooked) {
    if (hooked <= 0) return;
    if (hooked > hooked_) hooked_ = hooked;
    if (hooked > hooks_written_) hooks_written_ = hooked;
    status("the game has run " + std::to_string(hooked) + " hooks already; the next is " +
           std::to_string(hooked + 1));
}

bool Session::hook(std::string_view lua, std::string_view label) {
    const int n = (hooked_ > hooks_written_ ? hooked_ : hooks_written_) + 1;
    if (!writer_.hook(n, lua)) {
        status("hook " + std::to_string(n) + " could not be written anywhere the mod looks");
        return false;
    }
    hooks_written_ = n;
    status("hook " + std::to_string(n) + " written, " + std::string(label) +
           "; the game runs it within a second or two");
    return true;
}

void Session::restart() {
    talking_ = false;
    history_.clear();
    played_ = 0;
    written_ = 0;
    hooked_ = 0;
    hooks_written_ = 0;
    // Everything on disk is from a conversation that is over. Left there, the mod
    // walks forward while the next turn is readable and races through turns that
    // were said to somebody else.
    writer_.sweep(kSweepTurns);
    // The backend has not been restarted, so the world prefix is still cached and
    // the sheets still count as warm. Only the game went away.
    status("the game restarted, counting turns from one again");
}

}  // namespace moonshine
