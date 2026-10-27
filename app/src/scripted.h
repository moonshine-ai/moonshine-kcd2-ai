// The answers the menu makes predictable, written ahead of time and said without the model.
//
// Three of the rows on the opening menu are the app's own -- help, ask, about -- so what
// they ask is known before the player picks one, and the generator writes each soul's
// answer in the circle's context and in the bible's voice (data/kcd2/character-design/plan.md
// section 5.1). A giver's `help` answer is the offer itself; anyone else's is a refusal in
// character or a lead to the circle's giver; `about` is what they would tell a stranger about
// themselves; `ask` opens a short list of what they know, each item behind a trust tier the
// save decides (section 5.2). Said from the database, a reply is instant where a generated one
// waits on the prefill, and it is in voice where the 4B is not always -- and, for the offer,
// it takes the judge off the road: the row that accepts is written here too, so the errand is
// taken up on a pick rather than on a verdict about the model's own labelling.
//
// Everything here is decided when the conversation opens, like the quest plan, so the sheet
// and the answers stand still for its whole length.

#ifndef MOONSHINE_SCRIPTED_H
#define MOONSHINE_SCRIPTED_H

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "quest.h"

namespace moonshine {

// One thing a soul knows and would tell, from npc_knowledge: `trust` is open, friendly or
// confided, and gates the topic; `label` is what the menu calls it. A thing they only know
// once a favour has ended a certain way (npc_knowledge `after_quest`, `after_how`; Christine's
// kept paper, which is nothing to tell of when the prayer went down softened, notes.md 22 Oct
// 2026) names the quest and the endings (how_matches: `cursed`, `truth`, `lied|quiet`; empty
// for any ending), and is not offered -- not withheld, simply not theirs to know -- until the
// save says so.
struct Knowledge {
    std::string key;
    std::string label;
    std::string trust;
    std::string text;
    std::string after_quest;
    std::string after_how;
};

// One way of answering `help` when there is no favour of their own to offer, from
// npc_seed_answer. A reply that refers Henry to another soul's favour names its quest, and
// is only said while that favour is still there to be taken up.
struct HelpReply {
    std::string text;
    std::string refers_to_quest_id;
};

// The player's standing with a soul as the save has it, beyond the one quest's facts: how
// often they have met, where every favour in the save stands, and a trust number if a later
// pass has set one. The mod dumps all of it before the begin line.
struct Standing {
    // Conversations, counting the one that is opening: 1 is a first meeting, 0 means the save
    // said nothing, which is treated as a first meeting too.
    int met = 0;
    // Every quest the save has moved, by id, to "accepted", "handed" or "done". A quest not
    // here has not been touched, which is what makes a referral to it worth saying.
    std::map<std::string, std::string> quests;
    // The dispute's flags per quest, as the mod's letters: b for the player's offer to carry
    // the grievance, p for the giver's word given, r for it withheld.
    std::map<std::string, std::string> flags;
    // How each quest ended, by id, as the mod's store has it (`persuaded`, `lied` ...); a
    // quest not here ended plainly or has not ended.
    std::map<std::string, std::string> hows;
    // Game hours since each quest was taken up, by id, as the dump carries it after a
    // tilde; a quest not here has no accept time on record.
    std::map<std::string, double> ages;
    // How many of each quest's item the player carries, by id, off the begin line's
    // `holding=`; a quest not here was not counted.
    std::map<std::string, int> holding;
    // stats.trust from the save, or below zero when there is none.
    double trust = -1.0;

    bool first_meeting() const { return met <= 1; }
    // Untouched, which is all a referral by id can know.
    bool offerable(std::string_view quest_id) const;
    // The facts of one quest as the save has them: stage, flags and count. `have` is
    // `holding`'s if counted, else `fallback` -- the begin line's `have=`, which counts the
    // soul's first quest, for a mod that sends nothing more.
    QuestFacts facts_of(std::string_view quest_id, int fallback = -1) const;
    // Whether a quest's stage is "done".
    bool is_done(std::string_view quest_id) const;
};

// Below what a `confided` topic opens on the trust number alone.
inline constexpr double kConfidedTrust = 0.5;

enum class Trust { Open, Friendly, Confided };
// The tier the save allows with this soul: friendly on a second meeting or once a favour of
// theirs is done, confided once a favour is done and they have met three times, or on the
// trust number. `own` is the soul's own quest's facts.
Trust trust_tier(const Standing& standing, const QuestFacts& own);
// Whether a topic tagged `tag` opens at `tier`. A tag nobody recognises opens to no one.
bool trust_allows(Trust tier, std::string_view tag);
std::string_view trust_word(Trust tier);

// Whether what a soul knows (the knowledge base, knowledge.h) is theirs to give yet. A giver's
// knowledge is the help they offer for their favour -- "I'll offer you what help I can" is
// the reward where no money or goods are -- so until one of their favours is done, a question
// typed or spoken is put off (notes.md, 8 Oct 2026). A soul who gives no favour, or has had
// one done, answers as their trust allows.
enum class KbGate {
    Open,    // answer as the trust allows
    Asking,  // their favour is there to be asked: put off, and the ask put instead
    Owed,    // taken up and not yet done: put off, and reminded of what was agreed
    Shut,    // theirs to ask but not now (a chain's later link): put off, nothing asked
};
std::string_view kb_gate_word(KbGate gate);

// Somebody a soul might speak of: a neighbour in their circle, by the id the save knows
// them by and the given name their lines use, or somebody who is not in the game -- a dead
// husband, a clerk in another town (npc_absentee) -- by name alone. What a topic about a
// person is gated on, and what a line is read for (people_named).
struct Person {
    // The soul id, or `name:<name, lowercased>` for one who is not in the game. The token
    // the save keeps on `@heard` and dumps back as `known`.
    std::string token;
    // The given name as a line says it: "Alice", "Humphrey". A title is not a name
    // ("Father Ernest" is Ernest).
    std::string name;
};
// The given name out of a name as written: the first word that is not a title ("Father
// Ernest" is Ernest, "Old Hedvika" is Hedvika), without any possessive.
std::string given_name(std::string_view name);
// The token for `name`, for a person with no soul: name:<given, lowercased>.
std::string name_token(std::string_view name);

// A topic the player may ask about this conversation.
struct Topic {
    std::string key;    // npc_knowledge.topic_key, which the row's intent carries as ask:<key>
    std::string label;  // as written
    std::string text;   // the answer, said as it stands
    // The person the topic is about, as a Person::token, or empty for a topic about nobody
    // in particular (their work, the road). A row about a person is offered only once Henry
    // has met them or heard them named (notes.md 7 Oct 2026; Session::knows_of).
    std::string about;
};
// Whom a topic's label is about: the first word of it that is the given name of one of
// `people` ("Alice's panic", "Leonard the landlord", "Young Ludwig" -- the people's names
// decide, so a title or an epithet in front is passed over). A label that names somebody
// the soul's circle does not hold (label_names_someone) is about `name:<first word>`, so it
// is still gated on the name being said; a label naming nobody is about nobody.
std::string topic_person(std::string_view label, const std::vector<Person>& people);
// Everybody in `people` whose name is said in `words` as a whole word, by token, each
// once, in the order of `people`.
std::vector<std::string> people_named(std::string_view words, const std::vector<Person>& people);

// A remark Henry might make to this soul that is not a question about what they know -- on
// the work in hand, the look of them, the place -- with the soul's reply written beside it,
// from npc_remark. The row is Henry's line as written, and its intent carries the key as
// say:<key>. These fill the menu when nothing else written is left for it, so that a round
// is whole without the model (conversation-design plan section 2.5).
struct Remark {
    std::string key;   // npc_remark.remark_key
    std::string row;   // Henry's line, as the menu shows it
    std::string text;  // the soul's reply, said as it stands
    // Whether the remark is on the place the soul is standing -- "This cellar is barely fit
    // for an animal", "It smells of sour beer in here" -- and so only makes sense where their
    // routine puts them (npc_remark.place = 'here'). Met on the open road, those rows are
    // kept back (personadb.h, away_from_places); the rest say nothing about where they are.
    bool here = false;
    // Or on what the soul is doing as Henry walks up -- "Why are you praying out here?" --
    // and so only offered while the animation says they are (npc_remark.place names an
    // anim_activity category: 'praying', 'eating', ...; Approach::activity). Empty for the
    // rest.
    std::string doing;
};

// What the director can answer without the model this conversation. Any field may be empty,
// and an empty one puts that pick back on the model as before.
struct Scripted {
    // Said to `help` while this soul's own favour is there to be taken up: the offer, in the
    // giver's own words from the quest text.
    std::string offer;
    // Said to `help` when there is no favour to offer: a refusal in character, or a lead.
    std::string help;
    // Whether *Is there anything I can help you with?* belongs on the menu at all: this
    // soul has an errand of their own there to be taken, or `help` is a lead to somebody
    // else's that is still to be had. On everyone else the row is not offered (noted 4 Oct
    // 2026: a menu that asks every soul in town what Henry can do for them reads as a
    // questionnaire), though a typed offer of help is still answered with `help` as written.
    bool help_due = false;
    // The words of that row, where the quest wrote its own (`opener`: "Why are you so
    // unhappy?" to a widow who is plainly grieving, notes.md 7 Oct 2026); empty keeps the
    // stock "Is there anything I can help you with?". Same intent, same answer (`offer`).
    std::string help_row;
    // Said to `about`: the self-account on a first meeting, and after that the line written
    // for being asked again. With neither, the model answers, told not to repeat itself.
    std::string about;
    // Henry's line that takes the favour up, put first on the round after the offer with the
    // intent `accept`, so the errand is taken on a pick and the judge is not needed.
    std::string accept_row;
    // What the giver says to that: the written answer to Henry agreeing (`agreed`: thanks,
    // and where the thing or the person is to be found from where the giver stands; notes.md
    // 5 Oct 2026), or for a quest written before it the second telling of the offer
    // (`offer_2`, the mark on the thing, how the holder came by it). Said from here, so
    // taking the errand up asks the model for nothing; empty where neither was written, and
    // then the model answers as before.
    std::string accept_reply;
    // Henry's one question under the offer -- "Why did he throw you out?" -- with the intent
    // `more`, laid beside the accepting row on the round after the offer and kept on the list
    // until picked while the offer stands; and the giver's written answer to it, which adds
    // to the offer and settles nothing, so the accepting row is there again after it.
    std::string more_row;
    std::string more_reply;
    // His second question under the offer, where one was written ("You want me to lie to a
    // priest?" beside "Why?", In the Stocks): laid and answered as `more` is, under its own
    // intent `more_2`, so either may be asked first and the other stays.
    std::string more_2_row;
    std::string more_2_reply;
    // Said to `remind` while the giver's errand is taken up and not yet done: the offer
    // over again. The opening round's offer of help gives way to the remind row while this
    // is set (menu.h, kRemindRow).
    std::string remind;
    // A deliver's pry, while the writer's letter is in Henry's hand and not yet handed over:
    // Henry's row asking what it says (`pry`), and the writer's answer either way -- told,
    // grudgingly, when Henry's Speech carries it against the writer's at the quest's
    // difficulty (`pry_yes`; the game's DialogUtils.PersuadeByPlayer rule, his stat over
    // theirs by `pry_factor`), or refused (`pry_no`). Once a conversation; the check's result
    // goes to the mod as `@check speech pass|fail` for the game's own stat-check mark.
    std::string pry_row, pry_yes, pry_no;
    double pry_factor = 1.25;
    // What `ask` may open, at the trust the save allows, in the generator's order.
    std::vector<Topic> topics;
    // Henry's remarks with their answers, in the generator's order, each a row on its own.
    std::vector<Remark> remarks;
    // The self-account was said on an earlier meeting. With no line written for being asked
    // again, `about` goes to the model with a word not to repeat it.
    bool about_told = false;
    // Something is known but nothing opens at this trust, so `ask` is not offered: the
    // question would have nothing written behind it, and the model would brush it off or
    // invent an answer.
    bool withheld = false;
    // The tier the topics above were gated by, kept for the knowledge base (knowledge.h),
    // whose items carry the same tags and open to the same standing.
    Trust tier = Trust::Open;
    // Whether the knowledge base answers them at all this conversation, or is held back as
    // the help a giver's favour pays for (KbGate). The topics above are not gated by it:
    // they are the circle's written beats, and the ones about the favour are wanted while
    // it is open.
    KbGate kb_gate = KbGate::Open;
    // Everyone a line of theirs might name: the circle's people as given, and anyone a
    // topic is about who is in neither list, by name. What the session reads each line
    // for, so that a person named in Henry's hearing opens the questions about them.
    std::vector<Person> people;

    bool empty() const {
        return offer.empty() && help.empty() && about.empty() && topics.empty() &&
               remarks.empty() && remind.empty();
    }
};

// Decides the above from what is on file and what the save says. `quest` and `facts` are the
// favour this conversation is about and where it stands; a soul with none passes an empty
// quest. `others` and `others_facts` are the rest of the soul's favours, if any, which count
// for what they know that is no longer so and for the trust a finished favour earns.
// `remarks` are carried through as written, since nothing about the save changes them.
// `about_again` is what they say to "tell me about yourself" from somebody they have told.
// `people` are the circle's, for whom each topic is about (topic_person).
Scripted plan_scripted(std::string_view self_account, const std::vector<Knowledge>& knowledge,
                       const std::vector<HelpReply>& help_replies, const Quest& quest,
                       const QuestFacts& facts, const Standing& standing,
                       const std::vector<Quest>& others = {},
                       const std::vector<QuestFacts>& others_facts = {},
                       const std::vector<Remark>& remarks = {},
                       std::string_view about_again = {},
                       const std::vector<Person>& people = {});

// The words for the two sides of the `ask` round. The soul's own reply is the leave to ask --
// "Ask, then.", "What is it you want to know?" -- and names none of the topics, which the
// rows under it show (it used to recite them, "I can tell you about working the holding, my
// late husband, or sitting on the bench", which nobody says of themselves; notes.md 8 Oct
// 2026). Henry's row for one topic is "Tell me about your trade."; a label written in the
// third person ("his trade") is turned to face him. `opener` picks between a few ways of
// putting the reply, so two souls do not open it with the same words.
std::string ask_reply(const std::vector<Topic>& topics, unsigned opener);
std::string ask_row(const Topic& topic);
// The intent an ask row carries, and the key read back off one. Empty when it is not one.
std::string ask_intent(std::string_view key);
std::string_view ask_key(std::string_view intent);
// The same for a remark's row: say:<key>.
std::string say_intent(std::string_view key);
std::string_view say_key(std::string_view intent);
// A beggar's rows: alms:<kind>, kind one of money, food, clothes (menu.h, kAlmsRow). The
// kind is also the word the mod's @alms takes.
std::string alms_intent(std::string_view kind);
std::string_view alms_kind(std::string_view intent);

}  // namespace moonshine

#endif  // MOONSHINE_SCRIPTED_H
