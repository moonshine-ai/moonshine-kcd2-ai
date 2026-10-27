// A favour one soul asks and another soul is the object of, and what the app does about it.
//
// The generator deals a few hooks out as quests (npc_quest): a real item the graph puts in
// the holder's pocket, a payment when it comes home. The game's own quest graph owns the
// state that matters -- whether the journal entry exists, whether the money moved -- and this
// file is the director standing between the conversation and that graph. It decides what
// each party is told is true, asks the model one yes-or-no question about the player's words
// when something hangs on them, and writes the command the graph is owed into the turn file
// as a line the mod strips before speaking.
//
// The model never decides a fact. It is told the player has the axe, or has not, and it is
// asked only whether the player just agreed. The holder's side is not a conversation at all:
// while the quest is open the game offers its own dialogue menu on them -- persuade, threaten,
// pay, leave -- and weighs the checks itself, so the app only ever talks to the holder before
// the errand is taken up and after the thing has gone.

#ifndef MOONSHINE_QUEST_H
#define MOONSHINE_QUEST_H

#include <string>
#include <string_view>
#include <vector>

#include "approach.h"
#include "llm.h"

namespace moonshine {

// Which end of the favour this soul is. A soul may be on several quests -- the giver of a
// chain's every link, the holder of one thing and the asker of another -- in one role each.
// Onward is the third party of an ask whose word is carried on (Quest::carries_on): the one
// the giver sends Henry to with the truth, whose own menu is the game's and ends the errand.
enum class QuestRole { None, Giver, Target, Onward };

// One row of npc_quest, with the giver's written words joined on when there are any, and
// the slate's shape (npc_slate) when the circle pipeline dealt it.
struct Quest {
    std::string id;    // npc_quest.quest_id, which is also what the mod names its graph after
    std::string kind;  // "retrieve", "deliver" (a letter carried to the target), "find" (no
                       // target), "travel" (the target is looked at, and never spoken to),
                       // "beat" (the target is made to make a wrong right; knocked down as a last
                       // resort), "pray" (no target: a
                       // prayer said at a place), "mind" (the target is looked in on), "ask"
                       // (the target knows a truth the giver wants, and is got to tell it),
                       // "leave"/"return" (a thing put down at a place), "ambush" (the target
                       // gives a false way first) or "buy" (the target sells the item for a
                       // thing of the giver's and a sum)
    QuestRole role = QuestRole::None;
    std::string giver_soul, giver_name;
    std::string target_soul, target_name;
    std::string item_class, item_name;
    int reward_groschen = 0;
    std::string skill, difficulty;
    // From npc_quest_text, the giver's own lines for the two moments the director knows are
    // coming. Empty when the generation has not been run for the giver, in which case the
    // model is told the fact and left to find its own words.
    std::string name, offer, thanks;
    // The second telling of the offer (npc_quest_lines `offer_2`): what the first did not
    // say, the mark on the thing or how the holder came by it. The giver's answer when
    // Henry takes the errand up (scripted.h, accept_reply).
    std::string offer_2;
    // Taking it up (npc_quest_lines, 5 Oct 2026): `agreed` is the giver's answer when Henry
    // says he will -- thanks, and where the thing or the person is to be found from where
    // the giver stands -- and goes before `offer_2` as the accepting row's answer. `more`
    // is Henry's one question under the offer ("Why did he throw you out?") and `more_reply`
    // the giver's answer, which leaves the offer open (scripted.h, more_row / more_reply).
    std::string agreed, more, more_reply;
    // Henry's row that draws the offer out (npc_quest_lines `opener`, 7 Oct 2026): what he
    // asks in place of the opening round's "Is there anything I can help you with?" where
    // that question would be the wrong one -- "Why are you so unhappy?" to a widow who is
    // plainly grieving. Answered with `offer` as before; empty keeps the stock row.
    std::string opener;

    // The slate (data/kcd2/character-design/plan.md section 3). A chain is two favours from
    // one giver, the second not asked until the first is home; `after_quest_id` is the link
    // before this one, empty on the first. A nest is a favour whose holder will not part with
    // the thing until a favour of his own -- `requires_quest_id`, of which he is the giver --
    // is done. A dispute is a holder with a grievance against the giver, which the player can
    // carry between them instead of persuading, frightening or paying (bible section 6.2).
    // A story's later scene (data/kcd2/quest-design/scenes.md) is a link too, from any giver:
    // `after_quest_id` is the scene before, and `after_how` says which of its endings this
    // one follows -- tokens joined by `|` (`truth|lied`, `truth`), empty for any. A scene
    // whose scene before ended another way is void: never offered, and nothing says so.
    std::string chain_id;
    int chain_order = 0;
    std::string after_quest_id, after_item_name, after_how;
    std::string requires_quest_id;
    bool dispute = false;
    // What the giver has to give when the reward is something in kind rather than money:
    // a loaf, a rumour, or -- where they have nothing else -- what help they can give, which
    // is their answers to whatever Henry asks once the favour is done (scripted.h, KbGate).
    // Empty for money, and for a favour that pays in nothing
    // but itself (a nest's inner favour, whose payment is the holder letting the other
    // thing go), which the sheet puts as thanks.
    std::string reward_text;
    // The giver's lines for the dispute: Henry's putting the holder's condition to him, and
    // the giver's two answers to it. From npc_quest_lines; empty until written.
    std::string apologise, apologise_yes, apologise_no;
    // A deliver's lines at the writer's: Henry's row saying it is delivered (`report`, the
    // same words whether it is true), his row saying he kept or lost it (`keep`), and the
    // writer's answer to the latter (`kept_reply`; the answer to the former is `thanks`).
    std::string report, keep, kept_reply;
    // A deliver's pry (notes.md 9 Oct 2026, Holy Jest): Henry's row asking the writer what
    // the letter says (`pry`), and the writer's two answers -- told reluctantly when Henry's
    // Speech carries it (`pry_yes`), and refused when it does not (`pry_no`). Offered while the
    // letter is in Henry's hand and unhanded; the check is the app's, weighed from the two
    // Speech stats on the begin line (approach.h) at the quest's difficulty. Empty for a
    // writer who would tell nobody, and then no row.
    std::string pry, pry_yes, pry_no;
    // The letter back from the receiver (notes.md 9 Oct 2026, Holy Jest): the writer asks
    // what was said (`greet_handed`, his opening line with the letter delivered), and
    // Henry's three answers -- the truth (`told`; `report` serves when it is empty, and
    // `told_reply` is the writer's answer to it, `thanks` when that is) and two lies, a small one (`lie_soft`, "it is being looked into") and a large one (`lie_big`,
    // "he was horrified"), each with the writer's answer (`lie_soft_reply`, `lie_big_reply`).
    // All three complete the errand; the letter did arrive. Before the trip, `report` is
    // the lie it always was, now only after two game hours and on a Speech check; caught,
    // the writer says `lie_caught` and the errand stands.
    std::string greet_handed, told, told_reply, lie_soft, lie_soft_reply, lie_big, lie_big_reply, lie_caught;
    // A deliver whose receiver asked how Henry came by the thing (the mod's interrogation,
    // npcdb.hpp tell_*; In the Stocks' chalice, notes.md 9 Oct 2026): the store's how is the
    // answer he gave (`truth`, `pinned`, `covered`, `evaded`, `caught`), and the giver's
    // report row and answer follow it -- `told_<how>` is Henry's row saying what he said and
    // `told_reply_<how>` the giver's answer, each falling back to `told` / `told_reply` when
    // unwritten. Kept in the order of the hows above.
    struct Told {
        std::string how, row, reply;
    };
    std::vector<Told> told_by_how;
    // What stands between them once the errand is done, from this soul's side, over
    // anything else the sheet says of their situation (npc_quest_lines `since`, and
    // `since_<how>` for an ending; `since_target` / `since_target_<how>` for the other
    // party): Benesh, asked after the chalice was back whether he will have his bells again,
    // answered from a sheet that still had him accused (notes.md 14 Oct 2026). Read for the
    // soul's own role; `since_line` picks the ending's line over the plain one.
    struct Since {
        std::string how, text;  // how empty: the plain line
    };
    std::vector<Since> since_by_how;
    std::string since_line(std::string_view how) const {
        const Since* plain = nullptr;
        for (const Since& s : since_by_how) {
            if (s.how == how && !s.text.empty()) return s.text;
            if (s.how.empty()) plain = &s;
        }
        return plain ? plain->text : std::string();
    }
    // What the deliver carries (npc_quest_lines `item_kind`): empty for a letter, "thing"
    // for a thing of the game's own carried back to its owner (the chalice). The giver's
    // paragraph names it by `item_name` instead of "your letter", and no page is read.
    std::string item_kind;
    // A retrieve's lie with the thing in Henry's coat (npc_quest_lines, In the Stocks):
    // `give` is Henry's row handing it over (the hand-in, no longer on sight once a lie is
    // written beside it), `lie_have` his row saying the holder never had it -- said as a lie,
    // on a Speech check against the giver -- `lie_have_reply` the giver's answer when it
    // carries (the errand ends, unpaid, the journal's lied log) and `lie_have_caught` his
    // answer when it does not (the errand stands). Empty for a retrieve settled on sight.
    std::string give, lie_have, lie_have_reply, lie_have_caught;
    // Henry's second question under the offer (npc_quest_lines `more_2`, `more_2_reply`):
    // "You want me to lie to a priest?" beside "Why?". Laid with `more`, answered the same way.
    std::string more_2, more_2_reply;
    // A find's count: how many of the thing the giver wants, and the hand-in takes. From
    // npc_quest_lines; 1 for everything else.
    int count = 1;
    // A travel's lines at the giver's: Henry's three rows once he has been -- what he saw
    // (`tell`, paid, the plain completion), something else (`lie`, paid) and that he will
    // not say (`quiet`, unpaid) -- and the giver's answers to the last two (`lie_reply`,
    // `quiet_reply`; the answer to the truth is `thanks`). The lie is also on the menu
    // before he has been, since the giver cannot tell. An ask's are the same five, once the
    // one who knows has told (`handed`): the truth carried back, a lie, and silence.
    std::string tell, lie, quiet, lie_reply, quiet_reply;
    // A beat's and a mind's lines at the giver's: Henry's row that he knocked the man down
    // rather than getting his word (`fought`, a beat's paid completion when the store has him
    // beaten and no handover) and the giver's answer (`fought_reply`); Henry's row that the
    // man is dead (`confess`, unpaid) and the giver's answer (`dead_reply`). `report` is the
    // plain completion of all three thingless kinds, true or not: for a beat, that the man
    // has agreed to make it right. A pray's place, in the giver's words (`spot_text`).
    std::string fought, fought_reply, confess, dead_reply, spot_text;
    // The giver's opening lines for the two visits the errand makes distinct, so that neither
    // has to be generated on the press: the player back with the favour taken up and nothing
    // to show (`greet_owed`, any kind), and back carrying the thing without ever having agreed
    // to fetch it (`greet_settle_unasked`, a retrieve). The third such visit, back carrying it
    // as agreed, opens on `thanks`. From npc_quest_lines; empty until written.
    std::string greet_owed, greet_settle_unasked;
    // The giver's opening line on the first meeting, with the errand there to be offered
    // (`greet_offer`): what he says before Henry has asked him anything, so that the one
    // who hails Henry over has a reason ready when Henry gets there (Benesh, notes.md 13 Oct
    // 2026: he has heard Henry helps poor folk, and he has been ruined). Empty, and the
    // soul's own greeting serves as before. `thanks_onward` is the hand-in answer where the
    // errand leads on to another of this giver's: the thanks that do not close anything, said
    // before the next offer, in place of `thanks` -- which is written for the errand ending.
    std::string greet_offer, thanks_onward;
    // An ask whose word is carried on (npc_quest_lines `onward_soul`, `onward_name`): told the
    // truth, the giver pays and sends Henry to a third soul with it (Fox for Mutton's Heinz
    // to the master of the butchers' guild), and the errand ends there, on that soul's own
    // menu, as a word deliver's does -- one quest in the journal, not two (notes.md 26). The
    // giver's `thanks` is the sending; `greet_onward` their opening line to Henry back before
    // he has been. Empty for an ask that ends at the giver.
    std::string onward_soul, onward_name, greet_onward;
    // A find's keepsake (npc_quest_lines `keepsake_class`, `keepsake_name`; Forget Me Not's
    // carved wooden heart in the grass among the marigolds, notes.md 5 Oct 2026): a thing the
    // mod lays where the flowers grow and marks, and which Henry may bring back with them.
    // `thanks_keepsake` is the giver's hand-in answer when he has (QuestFacts::keepsake), in
    // place of `thanks`. Empty for every other find.
    std::string keepsake_name, thanks_keepsake;
    // A retrieve whose holder will not be moved (npc_quest_lines `unyielding`; In the Stocks'
    // sacristan, notes.md 13 Oct 2026): the game's rows on him are refusals, and the mod's
    // store carries the refusal as QuestFacts::rebuffed. Back at the giver with that, the
    // giver sends Henry to the holder's chest anyway and names the one who keeps the house
    // (`helper_name`, the mod's `helper_soul`'s given name): the opening line is
    // `greet_rebuffed`, and the naming (kQuestNamed) rides on Henry's row taking the chest
    // up (`named_row`, answered with `named_reply`; the plain words where none were written),
    // on which the mod opens her menu and marks her on the map. Empty for a holder who can
    // be talked round.
    bool unyielding = false;
    std::string helper_name, greet_rebuffed, named_row, named_reply;
    // An unyielding holder who can be caught where he should not be at night (npc_quest_lines
    // `seen_with`, `seen_with_name`; Words Must Not Fail's priest in the housekeeper's bed,
    // notes.md 21 Oct 2026): the mod's Watch notes the catching as QuestFacts::seen (flag s,
    // the travel's letter), and the game's menu on him then has the row that wins the thing
    // for Henry's silence (the `exposed` how). The giver, told the holder refused, sends
    // Henry after the holder's nights (`greet_rebuffed` here too); the holder's paragraph
    // turns on whether he has been caught. The name is the one he is caught with. Where the
    // mod's bedside is written, Henry may instead have walked out with nothing to denounce
    // him (QuestFacts::denounced): the giver's report row and answer for that are
    // `told_denounced` / `told_reply_denounced` (told_by_how), and the errand ends
    // `denounced` on them, nothing coming back; `exposed_paid` is the blackmail won.
    std::string seen_with_name;
    // The giver's hand-in answer when the thing came by the hold Henry found over the
    // holder (`thanks_exposed`, optional; QuestFacts::how `exposed` or `exposed_paid`): the
    // holder's silence was bought with more than the thing (Proof Against Her: the mass for
    // Christine's brother, notes.md 25 Oct 2026), and her thanks say so. `thanks` otherwise.
    std::string thanks_exposed;
    // The hand-in's thanks for the way the thing came: see thanks_exposed.
    const std::string& thanks_for(std::string_view how) const {
        if (!thanks_exposed.empty() && (how == "exposed" || how == "exposed_paid")) return thanks_exposed;
        return thanks;
    }
    // The giver's offer to carry Henry to where the errand is done and to its hour
    // (npc_quest_lines `fast_travel`, Henry's row; `fast_travel_reply`, the giver's answer;
    // `fast_travel_later`, Henry's row declining it, and `fast_travel_later_reply`; the
    // mechanics `fast_travel_to` "x,y,z", `fast_travel_at` "hh:mm" and `fast_travel_hours`
    // "from-to" are the mod's, not spoken). Proof Against Her (notes.md 25 Oct 2026): the
    // walk to the Kourim gate and the wait for the priest's homeward hour, saved. Laid as
    // rows after the yes, in place of the closing the yes would have been, and on every
    // later visit while the thing is still to be got; only between the hours the giver can
    // be asked it (the day's, so no night is skipped). The reply owes kQuestFastTravel, and
    // the mod does the carrying once the conversation is gone.
    std::string fast_travel_row, fast_travel_reply, fast_travel_later_row, fast_travel_later_reply;
    double fast_travel_from = 10.0, fast_travel_to = 19.0;  // `fast_travel_hours`, hours of the day
    bool has_fast_travel() const {
        return !fast_travel_row.empty() && !fast_travel_reply.empty() && !fast_travel_later_row.empty() &&
               !fast_travel_later_reply.empty();
    }
    // A retrieve of the very letter an earlier scene's dictation wrote (`item_of`: that
    // scene's quest id): the holder was brought it and kept it, he never borrowed it, and
    // his paragraph says so.
    std::string item_of;
    // The giver's answer to Henry giving the errand up (`give_up_reply`, optional: the model
    // answers in their own way without it), and the journal's line for it (`log_abandoned`,
    // the mod's; a plain line without it).
    std::string give_up_reply;
    // A deliver whose letter Henry writes at the giver's dictation (Words Must Not Fail,
    // npc_quest_lines dictate_*; the mod's npcdb.hpp has the same shape). Nothing is handed
    // over at accept: the giver says the letter a part at a time (`parts[k].dictate`), and
    // after each Henry has up to four rows, each with the giver's answer -- write it as said
    // (`write`), ask her whether it belongs in a prayer and leave it out (`reconsider`; not
    // written for a part that cannot be left out), soften it (`soften`) or make more of it
    // (`embellish`). The three outcomes make the letter the mod has a Document for
    // (kQuestWrote, the digits 0..3 per part), the receiver reads it part by part and gives
    // a verdict, and the writer's `told_<how>` rows follow it. `begin` is the giver's line
    // sitting Henry down before the first part, `done` the sealing after the last, `resume`
    // Henry's row picking the dictation up on a later visit and `greet_unwritten` the
    // giver's greeting then. The chosen row's answer and the next part are one reply in two
    // beats.
    struct Dictation {
        struct Choice {
            std::string row, reply;
            bool written() const { return !row.empty() && !reply.empty(); }
        };
        struct Part {
            std::string dictate;
            Choice write, reconsider, soften, embellish;
            // The outcome digits Henry can pick for this part, in menu order: 0 as said, 1
            // left out, 2 softened, 3 made larger (the mod's Quest::dictation_outcomes).
            std::vector<int> outcomes() const {
                std::vector<int> out = {0};
                if (reconsider.written()) out.push_back(1);
                if (soften.written()) out.push_back(2);
                if (embellish.written()) out.push_back(3);
                return out;
            }
            const Choice& choice(int outcome) const {
                return outcome == 1 ? reconsider : outcome == 2 ? soften : outcome == 3 ? embellish : write;
            }
        };
        static constexpr int kParts = 3;
        std::string begin, done, resume, greet_unwritten;
        Part parts[kParts];
    };
    Dictation dictation;

    bool exists() const { return !id.empty(); }
    // A deliver whose letter is written at dictation: every part said and writable as said,
    // and the giver's lines around them. The mod refuses a quest with some of these and not
    // all, so a half-written one never reaches the game.
    bool has_dictation() const {
        if (!is_deliver() || item_class.empty() || dictation.begin.empty() || dictation.done.empty()) return false;
        for (const Dictation::Part& part : dictation.parts) {
            if (part.dictate.empty() || !part.write.written()) return false;
        }
        return true;
    }
    // A retrieve whose giver has somebody to name once the holder has refused.
    bool has_helper() const { return unyielding && !helper_name.empty() && !greet_rebuffed.empty(); }
    // An unyielding holder with a secret to be caught in (above).
    bool has_expose() const { return unyielding && !seen_with_name.empty(); }
    bool has_keepsake() const { return is_find() && !keepsake_name.empty(); }
    bool carries_on() const { return is_ask() && !onward_soul.empty(); }
    bool pays_money() const { return reward_groschen > 0; }
    bool is_deliver() const { return kind == "deliver"; }
    // A deliver with no letter: word carried from the giver to the one it is for (Fox for
    // Mutton's last scene, the guild master told what the butcher sells). Nothing is made at
    // accept, the receiver's Talk is Henry telling him, and the mod completes the errand on
    // that row -- there is nobody to come back to with a seal to show -- so the giver's rows
    // are only there before it is told.
    bool carries_word() const { return is_deliver() && item_class.empty(); }
    // A deliver of a thing of the game's own, not a letter (item_kind "thing").
    bool carries_thing() const { return is_deliver() && !item_class.empty() && item_kind == "thing"; }
    // A retrieve whose giver can be lied to with the thing in Henry's coat: both the lie and
    // the giver's answer to it are written, so the hand-in is a row and the lie beside it.
    bool has_lie_have() const { return !lie_have.empty() && !lie_have_reply.empty(); }
    bool is_find() const { return kind == "find"; }
    bool is_travel() const { return kind == "travel"; }
    bool is_beat() const { return kind == "beat"; }
    bool is_pray() const { return kind == "pray"; }
    bool is_mind() const { return kind == "mind"; }
    bool is_ask() const { return kind == "ask"; }
    // A leave or a return (quest-design/set-aside-stories.md): a thing of the giver's carried
    // to a place and put down there; the giver's rows are the travel's three.
    bool is_leave() const { return kind == "leave"; }
    bool is_return() const { return kind == "return"; }
    bool is_spot() const { return is_leave() || is_return(); }
    // An ambush (set-aside-stories.md section 5): the target knows the way to somebody and
    // gives a false one first, to a place where men wait for Henry; the giver's rows are an
    // ask's. A buy: the target sells a thing (the item) for a garment of the giver's and a
    // sum, both carried from the giver; the giver's rows are the truth and a lie about the
    // price.
    bool is_ambush() const { return kind == "ambush"; }
    bool is_buy() const { return kind == "buy"; }
    // The kinds with nothing carried: taken up on a word and settled on a word. A leave's
    // thing is carried, but away from the giver, and nothing is ever brought to them; an
    // ambush's directions the same.
    bool thingless() const {
        return is_travel() || is_beat() || is_pray() || is_mind() || is_ask() || is_spot() || is_ambush();
    }
    // Whether the player has brought what the giver asked for: the one thing a retrieve is
    // about, or a find's full count. A letter is never "brought" to its writer.
    bool brought(int have) const {
        if (thingless()) return false;   // nothing is carried
        // A deliver's thing goes the other way: the letter, or the chalice the bell-ringer
        // hands over to go back to the priest, is never brought to the one who gave it.
        if (is_deliver()) return false;
        return is_find() ? have >= count : have > 0;
    }
};

// What is true of the quest when a conversation opens. The stages come from the app's own
// book, because they are things the app made happen; `have` comes from the game, which is
// the only party that knows what is in the player's pockets; the flags are the dispute's,
// kept by the mod's store as the conversations set them.
struct QuestFacts {
    bool accepted = false;  // the player agreed to the giver
    bool handed = false;    // the holder gave the thing up
    bool done = false;      // the giver has it back and has paid
    int have = -1;          // how many of the item the player carries; -1 when the mod did not say
    // The dispute. The player offered the holder to carry his grievance to the giver
    // (`broker`); the giver gave the holder his word (`apologised`) or would not (`refused`).
    bool broker = false;
    bool apologised = false;
    bool refused = false;
    // A travel: the player has been within sight of the one they were sent to look at.
    // The mod's Watch sets it; the dump carries it as the flag letter s.
    bool seen = false;
    // A beat: the man has been knocked down (flag k); a beat or a mind: he is dead (flag x).
    // A pray: the prayer has been said at the place (flag y). A mind: how many visits have
    // been made (one flag c each).
    bool beaten = false;
    bool dead = false;
    bool prayed = false;
    int checks = 0;
    // A leave or a return: the thing has been put down at the place (flag l), the cue from
    // the dark was heard after (h); a return's grave was robbed (o), and its curse is on
    // Henry now (u). The giver knows none of it; the rows turn on `left`.
    bool left = false;
    bool heard = false;
    bool looted = false;
    bool cursed = false;
    // An ambush: the target has given the false way (flag g), and the men at the place have
    // set on Henry (m); the true way given is `handed`, as an ask's truth is.
    bool directed = false;
    bool ambushed = false;
    // An ask whose word is carried on: the giver has been told, has paid, and has sent Henry
    // on to the onward soul (flag w). The errand is still open until that soul is told.
    bool onward = false;
    // A find's keepsake is in Henry's pocket (flag e): the giver's thanks at the hand-in are
    // `thanks_keepsake`. The mod's Watch sets and clears it.
    bool keepsake = false;
    // An unyielding holder's retrieve: the holder has refused Henry to his face (flag n), the
    // giver has named the one who keeps the holder's house (a), she has let Henry in by day
    // for his money (i), and she has stolen him the chest's key (q). The giver's rows turn
    // on the first two; the last two are hers and the mod's.
    bool rebuffed = false;
    bool named = false;
    bool letin = false;
    bool keyed = false;
    // A holder caught in the night and denounced at the bedside (flag d; notes.md 22 Oct
    // 2026): Henry walked out with nothing, and the thing is lost to the giver, who has
    // only his report to hear (the `denounced` how, settled by the mod at the row).
    bool denounced = false;
    // A letter written at dictation (Quest::has_dictation): the outcome digit of each part,
    // as the flag letters carry them after a W ("023"); empty until it is written. The
    // writer's rows for the letter itself (report, keep, the lie) wait on it.
    std::string wrote;
    // A later link of a chain whose earlier link is not yet home: the giver has the favour
    // in mind and does not ask it. Set by whoever knows the rest of the save (persona_for).
    bool locked = false;
    // A story's scene whose scene before ended a way this one does not follow: the giver
    // never has the favour to ask, and nothing is said of it. Set by persona_for.
    bool voided = false;
    // How the favour ended, as the mod's store has it and the dump carries it after a slash
    // (`persuaded`, `threatened`, `paid`, `stolen`, `brokered`, `nested`, `fought`, `lied`,
    // `kept`, `quiet`, `dead`); empty when the store has none, which a plain completion is too.
    std::string how;
    // Game hours since the favour was taken up, as the mod's store has it (the dump's
    // `~<hours>`); -1 when the store has no accept time (a mod from before 9 Oct 2026, or
    // not yet accepted). What keeps a letter's "I took it" off the list before Henry could
    // have been there and back (notes.md 9 Oct 2026, Holy Jest).
    double hours_since_accept = -1.0;
    // A pray: the player cannot pray at a shrine (the game's Basic Theology perk, read off
    // the begin line's pray=0), so the giver does not ask it of them. Set by persona_for.
    bool cannot_pray = false;

    // Whether the favour is there to be asked: nobody has agreed, nothing is in the player's
    // hands, and the link before it is done.
    bool offerable() const {
        return !accepted && !done && have <= 0 && !locked && !voided && !cannot_pray;
    }
};

// Whether a favour that ended `how` is one of the endings `wanted` names: tokens joined by
// `|`, each either an ending's own word or `truth`, which is every ending that is not
// `lied`, `quiet`, `kept` or `dead`, nor one of the interrogation's dodges (`pinned`,
// `covered`, `evaded`, `caught`). Empty `wanted` is any ending. The mod's QuestHowMatches
// (npcdb.hpp untrue_how), so the two cannot disagree about what a scene follows.
bool how_matches(std::string_view how, std::string_view wanted);
// The endings "truth" does not cover (the list above).
bool untrue_how(std::string_view how);

// The same, for a particular favour: a find is asked whatever the player happens to be
// carrying, since chamomile in Henry's bag is not the giver's chamomile until he agrees.
inline bool offerable(const Quest& quest, const QuestFacts& facts) {
    // A thing of the game's own carried back to its owner is asked whether or not one is in
    // Henry's pocket already: the chalice he just handed the bell-ringer is the chalice the
    // bell-ringer asks him to take on, and the count has not caught up with the hand-in.
    if (quest.is_find() || quest.thingless() || quest.carries_thing()) {
        return !facts.accepted && !facts.done && !facts.locked && !facts.voided && !facts.cannot_pray;
    }
    return facts.offerable();
}

// The commands a conversation can owe the game, as they appear after "@quest <id> " in a
// turn file and as the mod's Lua spells them.
inline constexpr std::string_view kQuestAccept = "accept";
// Not a quest command but a directive of the turn itself, beside `@more`: the conversation
// is to close once these words have been read (and any quest beat they carry has played).
// Written on the accepting turn, since the game's own quests close on the accepting line.
inline constexpr std::string_view kEndDirective = "@end";
// In front of every line an app with a voice writes: the words are being spoken out loud,
// so the mod paces the camera by the voice rather than by the reading time it guesses
// from the words. Henry's line is held, camera on him, until the app's hook
// (Moonshine.Spoken("henry")) says his audio is done, and the closing line (@end) keeps the
// conversation open until Spoken("npc") says the same -- each with a grace past the
// reading, for an app that said so and then did not. An app with no voice writes nothing
// here, and the mod paces by the reading alone as before.
inline constexpr std::string_view kVoicedDirective = "@voiced";
// Another directive of the turn itself: the speaker has more to say, and the next turn is
// their second line rather than Henry's. A written line told in two beats (a blank line
// between them in the quest text) is written as two turns, the first flagged so; the mod
// holds each for its reading and opens the menu after the last (notes.md 7 Oct 2026).
inline constexpr std::string_view kBeatDirective = "@beat";
// `@heard <token>`: the words of this turn name somebody Henry had not heard of, by the
// token the save keeps them under (scripted.h, Person::token) and dumps back as `known`
// (memory.h). Written ahead of the words that name them.
inline constexpr std::string_view kHeardDirective = "@heard";
// Splits a written line into its beats on blank lines; one beat when it has none.
std::vector<std::string> beats_of(std::string_view words);
inline constexpr std::string_view kQuestHandover = "handover";
inline constexpr std::string_view kQuestComplete = "complete";
// The giver's answer to the holder's condition, carried back by the player: his word given,
// or withheld. Facts for the store and the holder's menu, not journal moments.
inline constexpr std::string_view kQuestApologise = "apologise";
inline constexpr std::string_view kQuestRefuseApology = "refuse_apology";
// A deliver's two other endings, both said to the writer: Henry says it is delivered when it
// is still in his pocket (paid, the journal's lied log), or that he has not carried it and
// will not (unpaid, the kept log). Delivered is the plain `complete`, said with `report`.
inline constexpr std::string_view kQuestLie = "lie";
inline constexpr std::string_view kQuestKeep = "keep";
// A travel's unpaid ending: Henry went, and will not say what he saw. The truth is the plain
// `complete`, said with `tell`; the lie is `lie` again, paid the same.
inline constexpr std::string_view kQuestQuiet = "quiet";
// A beat's, a mind's and an ask's unpaid ending: the man is dead, and Henry says so. The
// plain `complete` is `report` (a beat's man agreed, a pray said, a mind's visits made), a
// beat's `fought` or an ask's `tell`; the lie is `lie` again, paid the same.
inline constexpr std::string_view kQuestConfess = "confess";
// An ask whose word is carried on: the truth told to the giver, who pays and sends Henry on
// to the onward soul. Said with `tell` in place of `complete`; the journal's objective is
// updated and the last one opened, and the errand completes on the onward soul's own menu.
inline constexpr std::string_view kQuestOnward = "onward";
// An unyielding holder's retrieve: the giver, told the holder refused, names the one who
// keeps the holder's house. Owed on the opening line (greet_rebuffed) the first time Henry
// is back with the refusal; the mod sets the store's a, opens her menu and marks her.
inline constexpr std::string_view kQuestNamed = "named";
// Henry giving an errand up to the giver's face (the "I give up" row on every list while
// one of theirs is open; notes.md 13 Oct 2026): the mod fails the quest in the book, the
// store has it done as `abandoned`, and the giver does not ask it again.
inline constexpr std::string_view kQuestAbandon = "abandon";
// The giver's carrying (Quest::fast_travel_row): once the conversation is gone the mod
// takes Henry to the errand's spot and winds the clock on to its hour (emit.cpp,
// FastTravelArm). Nothing in the store or the journal.
inline constexpr std::string_view kQuestFastTravel = "fast_travel";
// The row's words, and the plan's direction to the model when no answer was written.
inline constexpr std::string_view kGiveUpRow = "I give up. I can't do what you asked of me.";
// A letter written at dictation (Quest::Dictation): `wrote_<digits>`, one digit per part
// (0 as said, 1 left out, 2 softened, 3 made larger), owed on the giver's sealing line. The
// mod makes the Document of that combination in Henry's pocket, keeps the digits as the
// store's W flag, and sets the receiver's reading on them. No journal beat of its own: the
// token the mod makes with it updates the journal (log_written).
inline constexpr std::string_view kQuestWrote = "wrote_";
inline std::string wrote_command(std::string_view digits) { return std::string(kQuestWrote) + std::string(digits); }
// The intents of the dictation's rows (menu.h): `dictate` is Henry picking the dictation up
// (the giver begins, or begins again), and `write:<k>`, `reconsider:<k>`, `soften:<k>`,
// `embellish:<k>` his four ways with part k (1-based); `dictation_part` reads k and the
// outcome digit back off one, (0, -1) when it is none of them.
inline constexpr std::string_view kDictateIntent = "dictate";
// The row's plain words, where the writer gave it none (Quest::Dictation::resume).
inline constexpr std::string_view kDictateRow = "Let's get your letter written. Tell me what to put.";
std::string dictation_intent(int part, int outcome);
std::pair<int, int> dictation_part(std::string_view intent);

// A row of the menu the director writes itself, with the reply already decided and the
// command that reply owes the game. The dispute's apologise row is the older shape of the
// same thing and keeps its own fields below.
struct DirectorRow {
    std::string intent;   // one of menu.h's kDirectorIntents
    std::string text;     // Henry's words on the row
    std::string reply;    // the soul's written answer
    std::string command;  // what the answer owes the game; empty for nothing
    // A row that has to be carried off: a Speech check the session weighs as it weighs the
    // pry (scripted.h, pry_factor), Henry's stat over the soul's by `check_factor`. Lost, the
    // soul says `fail_reply`, owes nothing, and the row alone is spent. Empty for no check.
    std::string check;
    double check_factor = 1.25;
    std::string fail_reply;
    // A row for after the yes (the giver's carrying, Quest::fast_travel_row): kept off the
    // list while the errand's question is still open, laid on the one after it is taken up,
    // and the yes does not close the conversation while such a row waits.
    bool after_yes = false;
    // The reply is the last word: the conversation closes on it, command or none.
    bool closes = false;
    // Spent alone, with the rows that share its `group` (empty: the rest of the
    // director's rows go with it, the errand being settled; see Session::written_answer).
    std::string group;
};

// Everything the director decided for one conversation, fixed when it opens so the sheet
// stays the same text for its whole length and the cache is never fought over.
struct QuestPlan {
    // The paragraph for the character sheet. Empty when this soul has no quest.
    std::string block;
    // Commands owed to the game before a word is said: the giver seeing the axe in the
    // player's hands pays up on the first turn, and a payment made before the quest was
    // accepted has to accept it first for the graph's sake.
    std::vector<std::string> opening_commands;
    // For the giver: the one thing the player's words could change, put as a question the
    // model can answer YES or NO to. Empty when nothing hangs on what they say.
    std::string question;
    // The command a YES fires, and the bracketed direction added to the player's words so the
    // reply reflects what just became true.
    std::string on_yes;
    std::string direction;
    // The dispute, once the player has offered the holder to carry it: Henry's row that puts
    // the holder's condition to the giver, the giver's written answer to it, and the command
    // that answer owes (kQuestApologise or kQuestRefuseApology). All empty when there is no
    // condition to carry, or the giver has already answered it.
    std::string apologise_row;
    std::string apologise_reply;
    std::string apologise_command;
    // The deliver's rows at the writer's: `report` and, while the letter is still about,
    // `keep`. On every list until picked.
    std::vector<DirectorRow> rows;
    // The giver's opening line written for this very visit, when the errand makes the visit
    // one of its own: the player back with the favour open (`greet_owed`), or carrying the
    // thing (`thanks`, or `greet_settle_unasked` when nobody agreed to anything). Said in
    // place of the state greeting, with the opening commands riding on it; empty when the
    // visit is an ordinary one or the line was never written, and the greeting is the
    // state's or -- on a settling turn -- generated.
    std::string greeting;
};

// `met` is how many conversations this makes with the soul, for the dispute's answer; 0 is
// as good as a first.
QuestPlan plan_quest(const Quest& quest, const QuestFacts& facts, const Approach& approach,
                     int met = 0);

// Which of a soul's quests this conversation is about, when they are party to more than one:
// the one the player has brought the thing of, else the one they are on, else the one there
// to be asked, else the one they hold the thing of, else the first. `facts` is indexed like
// `quests`. Returns the index, or quests.size() when there are none.
std::size_t pick_quest(const std::vector<Quest>& quests, const std::vector<QuestFacts>& facts);

// One sentence about a quest the conversation is not about, for the sheet, so a soul on two
// favours is not made to forget the other: the thing they still hold, the errand still out.
// `all` and `all_facts` are the soul's every quest, for a nest's inner favour to be named
// from the outer. Empty when the other favour is not worth a word yet -- a chain's later
// link before its turn.
std::string quest_aside(const Quest& quest, const QuestFacts& facts,
                        const std::vector<Quest>& all, const std::vector<QuestFacts>& all_facts);

// Whether the giver gives his word when the player puts the holder's condition to him. On
// the player's bearing (the mod's relative charisma, how well dressed) against the quest's
// difficulty, or on having been asked enough times; a giver whose bearing the mod did not
// report gives it, since the pilot has to be playable on any save.
bool giver_relents(const Quest& quest, const Approach& approach, int met);

// The line a turn file carries for one command, which the mod reads and does not speak.
std::string quest_directive(const Quest& quest, std::string_view command);

// The judge's verdict on the player's words, read off a reply that was asked for one word.
enum class Verdict { Yes, No, Unclear };
Verdict read_verdict(std::string_view reply);

// The conversation as it would be sent for the reply, with the last user turn rewritten to
// carry the question. Shares every byte of the prefix with the reply that follows, so the
// cost is the question and a token or two of answer.
std::vector<Message> judge_messages(std::vector<Message> conversation, std::string_view question);

// Sampling for a verdict: cold, and a handful of tokens.
Options judge_options(const Options& options);

}  // namespace moonshine

#endif  // MOONSHINE_QUEST_H
