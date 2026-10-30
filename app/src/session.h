// One conversation, and the accounting that keeps it in step with the game.
//
// The game and this app each keep a turn number, and they have to agree. Two
// things about the game's counter are not what you would guess, and both are
// pinned by tests rather than remembered:
//
// It counts across the whole run of the game, not per conversation. The second
// person the player talks to starts at whatever turn the first one finished on,
// because the file names have to stay unique for as long as the game is open --
// the engine caches what it reads by path.
//
// It only ever moves when a turn is played. A turn written is not a turn heard,
// so nothing here may advance on its own: the next turn is owed only once the
// previous one has been reported played.

#ifndef MOONSHINE_SESSION_H
#define MOONSHINE_SESSION_H

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gamelog.h"
#include "knowledge.h"
#include "llm.h"
#include "menu.h"
#include "prefill.h"
#include "prompt.h"
#include "turnfile.h"

namespace moonshine {

// Who the player is talking to, looked up by the soul id the mod logged -- an id like
// "kkut_additive_man_47", not a name. Returning nothing means talk anyway with a
// stranger's sheet, because a conversation that refuses to happen is worse than a
// vague one.
// `anim` is the Mannequin name the mod last saw them playing, resolved to words by
// whoever supplies the persona rather than here.
// `record` is what the save remembers of this soul and the player, as the mod dumped it
// just before the begin line: how often they have met, where the quest stands, and the
// earlier conversations. Empty when the save has no record of them -- a first meeting, or
// a mod too old to keep one -- and the supplier treats both as a stranger's first time.
using FindPersona = std::function<std::optional<Persona>(
    std::string_view soul, const Approach& approach, std::string_view anim,
    const SoulRecord& record)>;

// Somewhere for the app to watch a conversation happen: the transcript, the
// status line, and the speaker.
struct Listener {
    // Each speakable piece of the NPC's reply, as it is ready. This is where TTS
    // hangs; it is called from whichever thread is running the turn.
    std::function<void(std::string_view)> on_say;
    // A line of the other party's that is known before the mod has shown it: the written
    // greeting, at the look and again as it is written on the press, one beat at a time.
    // Where the voice makes the audio ahead, so the line is spoken the moment the mod asks
    // for it rather than synthesized then. `actor` is the stand-in the soul speaks in, or
    // empty for the default.
    std::function<void(std::string_view text, std::string_view actor)> on_prepare;
    // The look has moved to another soul: whatever on_prepare asked for and the voice has
    // not made yet is not wanted (Speaker::forget_ahead).
    std::function<void()> on_drop_prepared;
    // A whole turn, once finished, for the transcript.
    std::function<void(const Message&)> on_turn;
    std::function<void(std::string_view)> on_status;
    // Every turn's cost, because prefill time regresses silently.
    std::function<void(const Timing&)> on_timing;
    // A quest command the session has just owed the game -- "accept", "handover",
    // "complete" -- by quest id. This is where the app's own book of quests is written,
    // since the session has no business knowing where that lives.
    std::function<void(std::string_view quest_id, std::string_view command)> on_quest;
    // A row of the reply menu, as it is accepted and sent to the game. The rows of one
    // round arrive in order, row 1 first; a new round starts at row 1 again.
    std::function<void(const MenuOption&)> on_option;
    // The menu's say row went live (true) or stopped being (false): where the microphone
    // opens and closes. What it hears comes back through Session::heard().
    std::function<void(bool)> on_listen;
};

class Session {
public:
    Session(Llm& llm, TurnWriter& writer, Prefiller& prefiller, Options options);

    void set_world(std::string world) { world_ = std::move(world); }
    void set_persona_source(FindPersona find) { find_ = std::move(find); }
    void set_listener(Listener listener) { listener_ = std::move(listener); }
    // How often a reply being generated is sent to the screen. Each update is a turn
    // file of its own -- the engine caches a path once read, so the text so far cannot
    // be rewritten in place -- flagged @more so the mod knows to replace the line rather
    // than start a new one and to keep the keyboard shut. Zero sends every token; a very
    // long interval sends only the finished reply.
    void set_stream_interval(std::chrono::milliseconds every) { stream_every_ = every; }
    // Whether to follow every line the NPC says with a menu of replies for the player to
    // pick from (menu.h). Off, the player types, and the mod is left to its keyboard.
    void set_menu(bool on) { menu_ = on; }
    // Whether the app speaks the lines it writes. On, every line goes out with @voiced in
    // front, which tells the mod to hold the camera for the voice rather than for the
    // reading time it guesses from the words, and spoken() tells it when a line's audio has
    // finished.
    void set_voiced(bool on) { voiced_ = on; }
    // A line the voice is done with: heard to the end, or dropped. Two lines matter --
    // Henry's latest, which the mod keeps the camera on until it hears this, and the
    // closing words of a conversation, which it stays open for -- and each goes back as a
    // hook calling Moonshine.Spoken("henry") or ("npc"), once. Any other line is nothing to
    // it. Matched with whitespace flattened, as the voice marker carries them.
    void spoken(std::string_view text);
    // The microphone's transcript so far while the say row is live, as the transcriber has
    // it (the whole line, replaced each time). Sent to the mod as a hook calling
    // Moonshine.SayHeard, which puts it in the slot; not more often than every kHeardEvery,
    // since the mod runs one hook a tick, and only when the words have changed. The
    // player's A on the slot is what sends it on -- it comes back as a Said line -- so
    // nothing here answers anything. Ignored when the row is not live.
    void heard(std::string_view text);
    // Whether the say row is live: between SayStart and SayStop.
    bool listening() const { return listening_; }
    // What the soul knows, for the lines the menu did not offer (knowledge.h). With one, a
    // typed or spoken line is scored against the soul's items and the model is told what
    // they know of it, or that they know nothing, or that they will not say yet; and a line
    // that offers help is met with the giver's ask before any of that. Null, and the model
    // answers from the sheet alone, as it did before. Not owned.
    void set_knowledge(KnowledgeDirector* knowledge) { knowledge_ = knowledge; }

    // What the mod said. Drives everything: opens and closes conversations and
    // keeps the turn counter in step.
    void handle(const Event& event);

    // The game relaunched, so its counter is back to nought and any turn files
    // still on disk are words from a conversation that is over.
    void restart();

    // What the player just said. Answering is what produces the next turn, and it
    // is only written once the game has played the one before it.
    //
    // Words that reach the app directly -- typed at its console, or heard by its
    // microphone -- are words the game never saw, so they are sent back to it as a
    // directive on the reply's turn file for the save to remember. Words the game itself
    // reported, off its own keyboard, it has already remembered; `heard_by_game` says which.
    void say(std::string_view said, bool heard_by_game = false);

    bool talking() const { return talking_; }
    const std::string& npc() const { return persona_.name; }
    // The voice-bank abbreviation for whoever is being spoken as, or empty.
    const std::string& actor() const { return persona_.actor; }
    // The game's actor code for the soul, whose facial clips move the mouth (lipsync.h).
    const std::string& face() const { return persona_.face; }
    // The highest turn the game has reported playing.
    int played() const { return played_; }
    // Join a game already running: the turns it has played so far, read off the log it
    // wrote before this app opened it (main.cpp). The mod counts turns across the whole
    // run, so an app that starts late and counts from one writes turn_1 while the mod
    // waits for turn_N+1 -- which is a conversation that never answers. Nothing to do when
    // the log has nothing played yet.
    void resume(int played);
    // The same for the hooks: the highest hook_N.txt the game reports having run this run.
    void resume_hooks(int hooked);

    // Lua for the game to run (turnfile.h, TurnWriter::hook; emit.cpp, Hook and Console):
    // the console's teleports. Numbered like a turn, across the run: one past the game's
    // count or this app's, whichever is ahead, so two typed within a second are two files
    // and the mod runs them in order. `label` is for the status line. False when no path
    // took the file.
    bool hook(std::string_view lua, std::string_view label);
    // The highest hook the game has reported running.
    int hooked() const { return hooked_; }
    const std::vector<Message>& history() const { return history_; }
    // The record the save last dumped, whole or still arriving.
    const SoulRecord& record() const { return record_; }
    // The plan the conversation is on, for tests of a plan changed within one (settled()).
    const Persona& persona() const { return persona_; }
    // Directives waiting for a turn file to ride on.
    const std::vector<std::string>& pending() const { return pending_; }
    // The rows offered after the last line, in row order; empty until the model has
    // written one, and between conversations.
    const std::vector<MenuOption>& rows() const { return rows_; }

    // Whether to work out the model's replies to the rows on offer before the player picks
    // one (conversation-design plan section 2.5), and the opening line of a soul the player
    // is looking at before they press to talk (Marker::Look). Rows with a written answer
    // need none, and nor does a giver with a line written for the very visit; for the rest, a reply generated while the player reads the list, or the
    // face, is a reply said at once when they choose. Off by default, since it keeps the
    // model busy.
    void set_speculation(bool on) { speculate_ = on; }
    // How long a look has to last before anything is made ahead for it -- the greeting's
    // audio, the opening line's guess. A glance in passing starts nothing; a look held
    // this long is one the player may act on. Measured by speculate(), which the poller
    // calls between looks at the log. Zero makes the look-ahead immediate (the tests).
    void set_look_dwell(std::chrono::milliseconds dwell) { look_dwell_ = dwell; }
    // Whether there is a row on offer, or a soul looked at, whose reply could be guessed
    // and has not been.
    bool has_speculation() const;
    // Works on the next such reply until it is done or `interrupted` says to stop, which
    // is checked as each token arrives and, on a backend that can, between pieces of the
    // prefill. Meant to be called from the same thread and under the same lock as handle()
    // and say(), whenever they have nothing to do: nothing else may use the model while it
    // runs. Returns whether there is more to guess, so the caller knows to come straight
    // back rather than sleep.
    bool speculate(const std::function<bool()>& interrupted);
    // How many guesses were used, and how many were made and not used, this run. Opening
    // lines count with the rows.
    int guesses_used() const { return guesses_used_; }
    int guesses_wasted() const { return guesses_wasted_; }

private:
    void open(std::string_view soul, const Approach& approach, std::string_view anim);
    // The player is looking at a soul and has not pressed to talk. Plans their opening line
    // as a guess, if it would otherwise be the model's to write when they do.
    void look(std::string_view soul, const Approach& approach, std::string_view anim);
    // The opening list for `persona`, written as the turn after the next greeting so the
    // files are already in the inbox when the player presses (notes.md: the "..." flash).
    // offer() rewrites the same turn on the press, so a look at someone else cannot leave
    // the wrong list on disk.
    void preview_opening_list(const Persona& persona);
    // The written greeting, beat by beat, to the voice to make ahead (Listener::on_prepare)
    // -- the words the mod will show if the press comes to the written line. At the look,
    // so it is made by the time the player presses; and as it is written on the press, for
    // the press no look came before, so making it starts when the words are decided rather
    // than when the mod has them on screen (measured 4s later on the Windows laptop, 11 Oct 2026).
    void prepare_greeting(const Persona& persona);
    // The persona for a begin or a look, from the source, against the record the mod dumped
    // ahead of the line if it is this soul's -- the one lookup both make, so a guess made on
    // the look is against the very sheet the begin will have.
    Persona persona_for(std::string_view soul, const Approach& approach, std::string_view anim);
    void greet();
    void close();
    // After a conversation: when the oldest remembered lines are too many, plans their
    // folding into a paragraph (fold_), which speculate() does between looks at the log.
    // Not done here, on the loop that answers the game: it took 35-43 s on the Windows
    // laptop for a record of 34 entries, and every soul pressed on in that time got the
    // camera and no words (29 Oct 2026, after Words Must Not Fail).
    void compact();
    // The planned fold, up to `interrupted`: the model asked for the paragraph, and the
    // directive that tells the save queued for the next turn file. Returns whether it was
    // interrupted and is still to do.
    bool fold(const std::function<bool()>& interrupted);
    // The look-ahead between conversations -- the voice on the written greeting, the model
    // on the opening line -- for a look that has lasted. Nothing when there is none to do;
    // else speculate()'s answer, whether there is more.
    std::optional<bool> look_ahead(const std::function<bool()>& interrupted);
    // Generates a reply and writes it as the next turn. Blocks for as long as the
    // model takes. `remember` sends the player's words to the save first.
    void answer(std::string_view said, bool remember = false);
    void status(std::string_view text);
    // Asks the model the plan's one question about what the player just said, and if the
    // answer is yes owes the game the command and returns the direction to add to the
    // player's words. Empty when nothing was asked or the answer was not yes.
    std::string judge(std::string_view said);
    // A command to the game, queued for the next turn file and reported.
    void owe(std::string_view command);
    // The knowledge direction for the player's words, when there is a director: the giver's
    // ask for a line that offers help (`offering`), else what the soul knows of what was
    // said. Empty direction when there is nothing to add.
    Advice advise(std::string_view said, bool offering);
    // Whether a typed line is the player offering to help, by the director's reckoning.
    bool offers_help(std::string_view said);
    // Where the giver's favour stands for a line that offers help.
    AskState ask_state() const;
    // Records what the reply just said drew on, for the follow-up reading of the next line.
    void drew(std::string item_id, std::string_view reply);
    // Asks the model for the four replies the player will choose between after the line
    // just said, and sends each to the game as it passes the checker. Blocks like answer().
    void offer();
    // The rows written for this soul that are not yet spent, up to `room` of them, in the
    // order they should fill a list: the seeds `help` and `about` while their answers are
    // still to be said (not on the opening round, where they are already there), then what
    // they know and the remarks written for them, turned so that a row passed over is not
    // offered in the same place every round. Every row here is one answer_scripted() has
    // an answer for.
    std::vector<MenuOption> written_fill(bool first_round, int room);
    // The words of the `help` row for this soul: the quest's own opener where it wrote one
    // (Scripted::help_row), else the stock "Is there anything I can help you with?".
    std::string help_row() const;
    // Whether a topic may be offered: one about nobody always; one about a person only once
    // Henry has met them or heard them named, by the save (record_.known) or this
    // conversation (heard_) (notes.md 7 Oct 2026: no asking after a stranger's neighbours).
    bool knows_of(const Topic& topic) const;
    // Whether "I have a question" has anything behind it: a topic that is open and known.
    bool ask_due() const;
    // Whether the row is left off the list: something is written and none of it is open.
    bool nothing_to_ask() const;
    // Reads a line the soul is about to say for the people it names, remembers them for
    // this conversation and owes the save `@heard` for each one new to it. Before the
    // line is written, so the directive rides on the turn that said the name.
    void note_heard(std::string_view words);
    // What a picked row would be answered with from what was written for the soul
    // (Persona::scripted) -- the offer, the self-account, the list of what they know, one
    // topic, a remark -- and everything saying it entails: the rows laid for the round
    // after, the commands owed the game, the keys spent. Nothing when the pick has no
    // written answer at this point of the conversation, and the model answers it.
    struct WrittenAnswer {
        std::string reply;
        std::string what;  // for the status line
        std::vector<MenuOption> next;
        std::vector<std::string> commands;
        std::vector<std::string> spent;
        // Raw directives the mod is owed with the words (@alms <kind>), ahead of the reply.
        std::vector<std::string> directives;
        bool ask_round = false;
    };
    std::optional<WrittenAnswer> written_answer(std::string_view intent) const;
    // Henry's rows for what he could give a beggar, by kind, in the begin line's letter
    // order (money, food, clothes); empty at anyone who is not begging.
    std::vector<std::pair<std::string, std::string>> alms_rows() const;
    // Says the written answer to the picked row, if there is one, then offers the rows
    // after it. False when nothing was written for that pick, and the model answers as it
    // always did.
    bool answer_scripted(std::string_view said, bool remember);
    // The errand settled on a row of the director's (told, lied about, kept quiet, confessed
    // lost), the save told so by the command owed: the conversation is planned again on that
    // ending. The reminder goes with the errand; and a story's next scene, planned locked when
    // the conversation opened, is this giver's to ask at once -- its offer is said in the
    // same breath as the thanks, the accepting row under it -- rather than waiting for Henry
    // to offer his help again or happen back (notes.md 8 Oct 2026: Sweepings and Strays,
    // "just talk and return", then the hand-over "easy to miss", when A Notched Ear was
    // written to follow it). In two halves around the reply: settle() keeps the record and
    // plans what follows, before the thanks are written (so the complete is owed on the
    // errand that ended); adopt() takes the plan up after.
    struct Settled {
        Persona next;
        // An errand of this giver's was ended on the command; false when there was none to
        // end (a holder's row, no errand), and then nothing below is meant.
        bool ended = false;
        // The plan is another errand of this giver's, with its offer ready to be said.
        bool leads_on = false;
        // The plan is this errand as done, or the next; nothing to adopt when neither.
        bool adopt = false;
    };
    Settled settle(std::string_view command);
    void adopt(Settled settled);
    // An ending settled by judge() on a row the model is answering, adopted once the reply
    // is out (the give-up row with no answer written).
    std::optional<Settled> settling_;
    // The words the model would be given if this row were picked now: the row's text with
    // the direction the director adds to it -- nothing when that direction would depend on
    // the judge, whose verdict cannot be known ahead.
    std::optional<std::string> heard_for(const MenuOption& row) const;
    // The direction the director adds to a pick's words for the model, or nothing.
    std::string direction_for(std::string_view intent) const;
    // Sets up a guess for every row on offer that the model would have to answer.
    void plan_guesses();
    // Forgets the guesses, counting the finished ones nobody picked.
    void drop_guesses();
    // Writes a reply that was not generated as the next turn, hands it to the voice in
    // pieces, and puts the exchange in the history as though the model had said it.
    // `heard` is what the history records as the player's turn when it is more than the
    // words -- the errand's direction after an accepting row -- so the model, asked later,
    // knows what was settled; the save remembers the words alone.
    bool say_written(std::string_view said, const std::string& reply, bool remember,
                     std::string_view heard = {});
    // The next turn number the game will accept: its own count where that is ahead of
    // what this app wrote, which it is after a restart and after any failed write.
    int next_turn() const { return (played_ > written_ ? played_ : written_) + 1; }
    // The words for a turn file: every owed command as a line the mod strips, then the words.
    // `closing` adds the @end directive when the conversation closes on these words; off
    // for a beat that is not the last of its line. A line of words (not a list of rows)
    // also gets @voiced when the app speaks.
    std::string with_owed(std::string_view words, bool closing = true);
    // "@end " when the conversation closes on the words; "@voiced " when the app speaks
    // them (set_voiced).
    std::string line_directives(bool closing) const;
    static std::string flat_words(std::string_view text);
    // Writes a written line as its beats (quest.h, beats_of), one turn each from `turn`:
    // the first carries what is owed, every beat but the last is flagged @beat so the mod
    // keeps the turn, and the last carries @end if the conversation closes on it. The last
    // turn written, or 0 when a write failed.
    int write_beats(int turn, std::string_view reply);

    Llm& llm_;
    TurnWriter& writer_;
    Prefiller& prefiller_;
    Options options_;
    Listener listener_;
    FindPersona find_;

    std::string world_;
    // The soul being talked to, by the id the mod logged; the knowledge base is keyed on it.
    std::string soul_;
    Persona persona_;
    std::vector<Message> history_;
    bool talking_ = false;
    KnowledgeDirector* knowledge_ = nullptr;
    // What each of the NPC's replies this conversation drew on, oldest first, with the reply:
    // an item of the knowledge base, the quest's ask, or nothing. For reading "please do"
    // against the offer it answers.
    std::vector<Drew> drew_;
    int played_ = 0;
    int written_ = 0;
    // The hooks, counted the same two ways: the highest the game reports having run and the
    // highest this app wrote. See hook().
    int hooked_ = 0;
    int hooks_written_ = 0;
    // A tenth of a second: the mod polls a growing line at about that rate, and a model
    // at 20 tok/s has two new tokens to show by then.
    std::chrono::milliseconds stream_every_{100};
    // Quest commands decided but not yet carried by a turn file. They ride on the next
    // words written, because the turn file is the only road into the game there is.
    std::vector<std::string> owed_;
    // The errand they were owed on. A hand-in that leads on adopts the giver's next scene
    // before the thanks go out, so by then quest_of is the next errand, not the one the
    // directive is for (the first in-game pass of 9 Oct 2026 completed the letter that
    // had never been accepted).
    Quest owed_for_;
    // The errand taken up this conversation: every turn from then on carries `@end`, so
    // the mod leaves once the giver's answer has been read and its accept beat played, the
    // way the game's own quests close on the accepting line; and no menu is offered.
    bool closing_ = false;
    // A letter being written at the giver's dictation (quest.h, Quest::Dictation): the part
    // Henry has just been given and has to choose his way with (1-based; 0 when nothing is
    // being dictated), and the outcome digits of the parts chosen so far. Each round's list
    // is that part's rows alone; the last part's choice owes the mod kQuestWrote with the
    // digits and closes the conversation on the sealing line. The errand accepted without
    // the giver beginning (a yes the judge read, rather than the accepting row) leaves the
    // dictation due, and the row that picks it up is laid first.
    int dictating_ = 0;
    std::string wrote_;
    bool dictation_due_ = false;
    // The dictation's rows for the part being chosen on; empty when none is.
    std::vector<MenuOption> dictation_rows() const;
    bool voiced_ = false;
    // The words the conversation closes on, as last written; empty until they are, and
    // again once the mod has been told they were spoken. And Henry's latest words, the
    // same way: the camera is his until his voice is done with them.
    std::string closing_words_;
    std::string henry_words_;
    // Whole directives for the save -- the player's words, a compaction -- waiting the same
    // way. Kept across conversations, since a compaction is decided after the last turn of
    // one and can only travel on the first turn of the next.
    std::vector<std::string> pending_;
    // The save's record of the soul being talked to, as the mod dumped it before the begin
    // line. Assembled from State and Memory events and kept until the conversation ends, so
    // that its length can be judged for compaction then.
    SoulRecord record_;
    // How the player walked up, and what the soul was playing, kept so the conversation can
    // be planned again on an ending settled in it (settled()).
    Approach approach_;
    std::string anim_;

    bool menu_ = false;
    std::vector<MenuOption> rows_;
    // Rows the app puts on the next round's list itself, ahead of whatever the model
    // writes: the line that accepts after a written offer, the topics after "I have a
    // question". Spent by the next offer().
    std::vector<MenuOption> preset_;
    // Topic keys already answered this conversation, so the next "I have a question" moves
    // on to what has not been said; and how many times it has been asked.
    std::vector<std::string> told_;
    int ask_rounds_ = 0;
    // Who has been named in Henry's hearing this conversation (scripted.h, Person::token),
    // read off every line the soul says as it is written (note_heard), so a question
    // about them opens on the very next list; the save is told on the same turn (`@heard`)
    // and has it back on the next dump. Cleared as a conversation opens.
    std::vector<std::string> heard_;
    // What Henry could give this soul, when they beg (approach.h, alms): the letters as
    // the begin line carried them, for the alms rows (menu.h, kAlmsRow).
    std::string alms_;
    // How far round the written rows written_fill() has turned this conversation.
    int fill_offset_ = 0;
    // The intent of the row the player picked, held until the words arrive on the said
    // line that follows the pick, where the director reads it instead of asking the judge.
    // Empty when the last words were typed rather than picked.
    std::string picked_intent_;
    // The say row: live between the mod's say start and say stop, the words last sent to
    // the slot, and when, so the hooks keep to the mod's pace (heard()).
    bool listening_ = false;
    std::string heard_text_;
    std::chrono::steady_clock::time_point heard_at_{};
    static constexpr std::chrono::milliseconds kHeardEvery{400};
    void set_listening(bool on, std::string_view why);

    // A reply worked out ahead for a row on offer: what the model would be told, and what
    // it said, once it has. Cleared with the rows, since a guess is against the history as
    // it stood when the list was offered.
    struct Guess {
        std::string intent;
        std::string text;
        std::string heard;
        std::string reply;
        Timing timing;
        bool done = false;
    };
    bool speculate_ = false;
    std::vector<Guess> guesses_;
    int guesses_used_ = 0;
    int guesses_wasted_ = 0;
    // The opening line worked out ahead for the soul the player was last seen looking at:
    // the persona as it was planned on the look, and what the model said as them, once it
    // has. Used on a begin whose sheet is the same one -- same soul, same state, same
    // standing -- and set aside on any other. Only ever one in hand, since the player can
    // only walk up to one of them: a look at somebody else replaces it, and a finished one
    // goes to kept_greetings_ below for when the look comes back.
    struct GreetingGuess {
        std::string soul;
        Persona persona;
        std::string reply;
        Timing timing;
        bool done = false;
        // Only the sheet to be read into the cache, for a soul whose line is written: the
        // work the written greeting used to leave for after itself.
        bool warm_only = false;
        // How often the model was stopped and started on it, and how many times the poller
        // took it up at all; for the status line on a press that finds it unfinished.
        int stops = 0;
        int runs = 0;
    };
    std::optional<GreetingGuess> greeting_guess_;
    // A record due to be folded, planned by compact() when its conversation closed and done
    // by fold() between conversations, after any look-ahead and never while one is open
    // (it would push the open soul's sheet out of the cache). Replaced by the next close
    // that finds a fold due; a fold set aside that way is asked for again the next time
    // that soul's record is dumped.
    struct PendingFold {
        std::string soul;
        std::string name;
        int rev = 0;
        Compaction due;
        // How often the model was stopped on it by a line of the log, for the status line.
        int stops = 0;
    };
    std::optional<PendingFold> fold_;
    // The look-ahead, held back until the look has lasted (notes.md 14 Oct 2026: walking
    // through a crowd, six souls were looked at in twelve seconds and each started the
    // voice on a greeting and the model on a line, beside the game, until the GPU driver
    // reset). look() plans; speculate() starts the voice on the beats below once
    // look_due_ has passed, and the model's guess after the voice. A press (open) or a
    // look at somebody else before then spends or replaces it.
    std::chrono::milliseconds look_dwell_{1500};
    std::chrono::steady_clock::time_point look_due_;
    std::string looked_soul_;
    struct PendingPrepare {
        std::vector<std::string> beats;
        std::string actor;
    };
    std::optional<PendingPrepare> look_prepare_;
    bool look_held() const;
    // Finished guesses the look has moved on from, oldest first, so that a gaze that goes
    // back and forth between two souls does not have each line made again at every turn of
    // the head: profiled on the Windows laptop (11 Oct 2026), Benesh Bell's line was guessed seven times in
    // four minutes, about 140 core-seconds each, while the player looked between him and a
    // neighbour. Keyed by soul and sheet, like the guess in hand, so a line fits only the
    // state it was made for. The hundredth oldest goes when a new one comes in.
    static constexpr std::size_t kKeptGreetings = 100;
    std::vector<GreetingGuess> kept_greetings_;
    void keep_greeting(GreetingGuess&& guess);
    std::optional<GreetingGuess> take_kept_greeting(std::string_view soul, std::string_view sheet);
};

}  // namespace moonshine

#endif  // MOONSHINE_SESSION_H
