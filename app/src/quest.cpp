#include "quest.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace moonshine {

namespace {

// Money without a figure. Nobody says a number aloud in this game -- not a count of things
// and not a sum -- so the sheet gives the model the weight of the money and never the
// amount: the mod pays the slate's groschen, the journal may print them, the mouth does not.
std::string groschen(int amount) {
    if (amount <= 0) return "nothing";
    if (amount < 6) return "a few groschen";
    if (amount < 15) return "a handful of groschen";
    if (amount < 40) return "a fair purse of groschen";
    return "a good purse of silver";
}

std::string in_quotes(std::string_view line) {
    return "\"" + std::string(line) + "\"";
}

// What the giver has to give, as it goes in a sentence after "give" or "pay": the money as
// words, or the slate's own phrase for a favour that pays in kind.
std::string reward_words(const Quest& quest) {
    if (quest.pays_money()) return groschen(quest.reward_groschen);
    if (!quest.reward_text.empty()) return quest.reward_text;
    return "your thanks";
}

// The threshold the player's bearing has to clear for a giver of this difficulty to swallow
// his pride on the first asking. Relative charisma is 0..1.
double relent_threshold(std::string_view difficulty) {
    if (difficulty == "easy") return 0.2;
    if (difficulty == "hard") return 0.6;
    return 0.4;
}

// How long a letter's writer gives Henry before "it is delivered" could be true: there and
// back in the same town is an hour or two of the game's clock.
constexpr double kLieHours = 2.0;

// The Speech check's factor by the favour's difficulty: Henry's stat over the soul's by this
// much carries it -- the shipped DialogUtils.PersuadeByPlayer's 1.5 for a hard one, gentler
// below (the same scale as scripted.h's pry_factor).
double check_factor(const std::string& difficulty) {
    return difficulty == "hard" ? 1.5 : difficulty == "easy" ? 1.0 : 1.25;
}

// The inner favour a nest's holder is waiting on, among the soul's quests, or null.
const Quest* inner_of(const Quest& outer, const std::vector<Quest>& all) {
    if (outer.requires_quest_id.empty()) return nullptr;
    for (const Quest& q : all) {
        if (q.id == outer.requires_quest_id) return &q;
    }
    return nullptr;
}

// Word carried instead of a letter (Quest::carries_word). The giver has something to be
// told to somebody he dare not face himself; Henry agrees or does not, and once he has told
// it the errand is done where it was told, so the giver only ever has the before of it: the
// asking, the waiting, and -- before the word is carried -- Henry saying he has carried it
// (a lie the giver cannot catch) or that he will not.
QuestPlan plan_carry_word(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    const std::string& to = quest.target_name;
    const std::string pay = reward_words(quest);
    const bool money = quest.pays_money();
    auto row = [&](std::string_view intent, const std::string& text, const std::string& reply,
                   std::string_view command, const std::string& plain_text,
                   const std::string& plain_reply) {
        plan.rows.push_back({std::string(intent), text.empty() ? plain_text : text,
                             reply.empty() ? plain_reply : reply, std::string(command)});
    };
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = "This person carried your word to " + to + " for you, or told you they "
                         "had, and that is settled between you" +
                         (quest.reward_text.empty() ? "." : "; " + pay + " was what you had for it.");
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person agreed to go to " + to + " and tell him what you dare not" +
                         (quest.reward_text.empty() ? "." : ", for " + pay + ".") +
                         " You have no way of knowing whether it has been said; you take their word "
                         "for it when they tell you. Ask how it went if you like.";
            if (!facts.handed) {
                row("report", quest.report, quest.thanks, kQuestLie,
                    to + " has heard it from me.",
                    "Has he. Then it is out of my hands, and good riddance.");
                row("keep", quest.keep, quest.kept_reply, kQuestKeep,
                    "I have not told him, and I am not going to.",
                    "Then it stays where it is. I asked; that is all I can do.");
            }
            return plan;
        }
        if (facts.locked) {
            plan.block = "You also have something you want carried to " + to +
                         ", but one thing at a time; you do not bring it up.";
            return plan;
        }
        plan.block = "There is something you want " + to + " told and dare not tell him yourself; "
                     "nobody has agreed to carry it yet. " +
                     (money ? "You would give " + pay + " to whoever does."
                            : quest.reward_text.empty()
                                  ? "You have nothing to give for it but your thanks."
                                  : "You have no money to give for it, but whoever does would have " +
                                        pay + ".") +
                     " If this person offers to, take them up on it and tell them exactly what " +
                     to + " is to hear and from whom.";
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to go to " + to +
                        " and tell him for you?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to carry it. You tell them again what " + to +
                         " is to hear, and from whom" +
                         (quest.reward_text.empty() ? ".]" : ", and that " + pay + " is theirs for it.]");
        return plan;
    }
    // The one to be told. He hears it in the game's own Talk on him, not in this conversation,
    // and knows nothing of it before.
    const std::string& from = quest.giver_name;
    if (facts.handed || facts.done) {
        plan.block = "This person came to you with word from " + from + ", which you have heard "
                     "and will deal with as you see fit; you do not chew it over with them.";
        return plan;
    }
    plan.block = "You expect nothing from " + from + " and know of no word from him. If this "
                 "person says they have something to tell you from him, tell them to say it "
                 "plainly, and no more; you are not one for talk with a stranger.";
    return plan;
}

// A deliver: the writer's side and the receiver's. The writer cannot see whether the letter
// went where he sent it, so nothing happens on sight; Henry has to say, and the two things he
// can say are rows the director writes (report, keep) with the writer's answer already
// decided. The receiver's handover is the game's own Talk on him, like the holder's menu.
QuestPlan plan_deliver(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    const std::string& to = quest.target_name;
    const std::string pay = reward_words(quest);
    const bool money = quest.pays_money();
    const bool carrying = facts.have > 0;
    if (quest.carries_word()) return plan_carry_word(quest, facts);
    auto row = [&](std::string_view intent, const std::string& text, const std::string& reply,
                   std::string_view command, const std::string& plain_text,
                   const std::string& plain_reply) {
        plan.rows.push_back({std::string(intent), text.empty() ? plain_text : text,
                             reply.empty() ? plain_reply : reply, std::string(command)});
    };
    // What is carried: a letter, or a thing of the game's own going back to its owner (the
    // parish chalice), which the paragraph names as the giver would.
    const bool thing = quest.carries_thing();
    // Or a letter the giver cannot write, which Henry writes at their dictation first.
    const bool dictated = quest.has_dictation();
    const std::string letter = thing ? "the " + quest.item_name : std::string("your letter");
    const std::string carry = thing ? "take " + letter + " back to " + to
                              : dictated ? "write your letter down and carry it to " + to
                              : "carry your letter to " + to;
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = thing
                ? "This person took " + letter + " back to " + to + " for you, or told you they had, "
                  "and that is settled between you"
                : "This person carried your letter to " + to + " for you, or told you they "
                  "had, and that is settled between you";
            plan.block += money ? "; you paid them " + pay + " for it." : "; you gave them " + pay + " for it.";
            return plan;
        }
        if (facts.accepted) {
            // A letter written at dictation and not yet written: nothing has left the house,
            // so none of the rows about the letter apply. The dictation itself is the
            // session's (Session::offer lays the part's rows); the paragraph says where
            // things stand, and Henry's one row here picks the dictation up.
            if (quest.has_dictation() && facts.wrote.empty() && !facts.handed) {
                plan.block = "This person agreed to write your letter to " + to + " down at your dictation "
                             "and carry it to him for " + pay + ", and it is not written yet: you have "
                             "not got past sitting them down. You give it to them a sentence or two at "
                             "a time, in your own words, and they may counsel you on each; you do not "
                             "let them put words in your mouth, though you may be talked round. "
                             "You give no directions to " + to + " until it is written and sealed; there "
                             "is nothing to carry yet. Nothing is owed until it is in " + to + "'s hand.";
                plan.rows.push_back({std::string(kDictateIntent),
                                     quest.dictation.resume.empty() ? std::string(kDictateRow) : quest.dictation.resume,
                                     quest.dictation.begin + "\n\n" + quest.dictation.parts[0].dictate, ""});
                return plan;
            }
            plan.block = "This person agreed to " + carry + " for " + pay +
                         ". You have no way of knowing whether it has reached him; you take "
                         "their word for it when they tell you, and give nothing until they do. "
                         "Ask how it goes if you like.";
            if (quest.has_dictation()) {
                plan.block += " They wrote it at your dictation, and sealed it with your thumb in the wax; "
                              "you know what it says, as near as you remember saying it.";
            }
            if (facts.handed) {
                // Delivered, by the receiver's own Talk. The writer wants to know what was
                // said, and Henry's answer is one of three rows: the truth (`told`, or the
                // plain report where none is written), paid; or a lie, small or large, paid
                // the same -- the letter did arrive, and the writer cannot know what was said
                // to it (notes.md 9 Oct 2026, Holy Jest). The lies say so on the row.
                // Where the receiver asked how Henry came by the thing, the store's how is
                // his answer, and the row and the reply written for that answer serve
                // (told_<how>, told_reply_<how>) over the plain pair.
                std::string told = quest.told, told_reply = quest.told_reply;
                for (const Quest::Told& t : quest.told_by_how) {
                    if (t.how != facts.how) continue;
                    if (!t.row.empty()) told = t.row;
                    if (!t.reply.empty()) told_reply = t.reply;
                }
                row("report", told.empty() ? quest.report : told,
                    told_reply.empty() ? quest.thanks : told_reply, kQuestComplete,
                    (thing ? "The " + quest.item_name + " is" : "Your letter is") + " with " + to + ".",
                    "Is it. Good. Here is what I said: " + pay + ".");
                if (!quest.lie_soft.empty() && !quest.lie_soft_reply.empty()) {
                    row("lie_soft", "(Lie) " + quest.lie_soft, quest.lie_soft_reply, kQuestComplete, "", "");
                }
                if (!quest.lie_big.empty() && !quest.lie_big_reply.empty()) {
                    row("lie_big", "(Lie) " + quest.lie_big, quest.lie_big_reply, kQuestComplete, "", "");
                }
                return plan;
            }
            // Not yet: the same words are a lie, and a lie that has to be carried off -- not
            // offered until two game hours have passed since the letter changed hands (there
            // and back takes that long; a store with no accept time withholds nothing), said
            // as a lie on the row, and weighed as a Speech check against the writer. Won, it
            // is paid as the truth would be; lost, the writer says so and the errand stands.
            // Or Henry owns that he has not carried it (or has lost it), and is not paid.
            if (facts.hours_since_accept < 0.0 || facts.hours_since_accept >= kLieHours) {
                row("report", "(Lie) " + (quest.report.empty()
                                              ? (thing ? "The " + quest.item_name + " is" : "Your letter is") + " with " + to + "."
                                              : quest.report),
                    quest.thanks, kQuestLie, "", "Is it. Good. Here is what I said: " + pay + ".");
                DirectorRow& lie = plan.rows.back();
                lie.check = "speech";
                lie.check_factor = check_factor(quest.difficulty);
                lie.fail_reply = quest.lie_caught.empty()
                    ? "You have never been near " + to + ". It is written all over you. Go, and come back when you have."
                    : quest.lie_caught;
            }
            row("keep", quest.keep, quest.kept_reply, kQuestKeep,
                carrying ? "I have not taken it to him, and I am not going to."
                         : "I have lost " + letter + ".",
                "Then give it back, or get out of my sight. I will find someone else.");
            return plan;
        }
        if (facts.locked) {
            plan.block = thing
                ? "You also have " + letter + " to send back to " + to + ", but one thing at a time; you do not bring it up."
                : "You also have a letter you want carried to " + to + ", but one thing at a time; you do not bring it up.";
            return plan;
        }
        plan.block = thing
            ? "You have " + letter + ", which belongs to " + to + ", and want it taken back and put in his "
              "hand by somebody else; nobody has agreed to yet. "
            : dictated
            ? "You cannot write, and you have a letter for " + to + " that you want written down at your "
              "dictation and then carried and put in his hand; nobody has agreed to yet. "
            : "You have written a letter to " + to + " and want it carried and put in his "
              "hand; nobody has agreed to yet. ";
        plan.block += (money ? "You would give " + pay + " to whoever does."
                             : quest.reward_text.empty()
                                   ? "You have no money to give for it, only your thanks."
                                   : "You have no money to give for it, but whoever does would have " +
                                         pay + ".") +
                      " If this person offers to, take them up on it, " + (dictated ? "" : "hand it over and ") +
                      (money ? "say there is money in it, without naming a sum." : "name what you can give.") +
                      (thing ? " You say how you came by it only as much as you would to a stranger."
                             : " You say what it is about only as much as you would to a stranger.");
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to " + carry + " for you?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = thing
            ? "[They have agreed to take it. You hand them " + letter + ", wrapped, and make sure they know it is " +
              pay + " when they come back.]"
            : dictated
            ? "[They have agreed to write it down and carry it. You sit them at the desk with the ink and "
              "the quill, and make sure they know it is " + pay + " when they come back; you do not begin "
              "dictating yet, and you give no directions to " + to + ": there is nothing to carry until it "
              "is written.]"
            : "[They have agreed to carry it. You hand them the folded letter, sealed, "
              "and make sure they know it is " + pay + " when they come back.]";
        return plan;
    }
    // The receiver. He knows nothing of any letter until it is in his hand, and the handing
    // over is the game's own Talk on him, not this conversation. The owner of a thing
    // carried back knows it is gone and nothing of its coming; the game's Talk is where he
    // asks how it was come by, and this conversation is either side of that.
    const std::string& from = quest.giver_name;
    if (facts.handed || facts.done) {
        plan.block = thing
            ? "This person brought your " + quest.item_name + " back to you, and you have it. How they came by "
              "it is settled between you, however it was settled, and you do not go over it again."
            : "This person brought you a letter from " + from + ", which you have. What "
              "it says is between you and " + from + ", and you do not read it out to them.";
        return plan;
    }
    plan.block = thing
        ? "Your " + quest.item_name + " is gone, and you know nothing of this person having it. If they say "
          "they have it, tell them to hand it over, and no more; you are not one for talk with a stranger."
        : "You know of no letter from " + from + " and expect none. If this person says "
          "they have one for you, tell them to hand it over, and no more; you are not "
          "one for talk with a stranger.";
    return plan;
}

// A find: the giver wants several of a common thing nobody holds, and the only other party
// is the town. Nothing to judge but the agreeing; the hand-in is on sight, like a retrieve's,
// once the errand is taken and the count is in the player's hands.
QuestPlan plan_find(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    if (quest.role != QuestRole::Giver) return plan;
    const std::string things = std::to_string(quest.count) + " " + quest.item_name;
    const std::string pay = reward_words(quest);
    const bool money = quest.pays_money();
    if (facts.done) {
        plan.block = "This person brought you the " + things + " you asked for, and " +
                     (money ? "you paid them " + pay + " for it."
                            : "you gave them " + pay + " for it.") +
                     " That is settled between you, and you think the better of them for it.";
        return plan;
    }
    if (facts.accepted && quest.brought(facts.have)) {
        plan.opening_commands.emplace_back(kQuestComplete);
        plan.block = "This person has brought you the " + things + " you asked for, as they agreed "
                     "to. Thank them, in your own way, and " +
                     (money ? "pay them the " + pay + " you promised."
                            : "give them what you said you would: " + pay + ".") +
                     " Where they got them is not your business and you do not ask.";
        // The keepsake brought back with them (the flag e): the thanks the writer gave the
        // giver for that, which say what it is and what it means to them.
        const bool with_keepsake = quest.has_keepsake() && facts.keepsake;
        if (with_keepsake) {
            plan.block += " They have also brought the " + quest.keepsake_name +
                          " that lay in the grass where the flowers grow, which you did not ask "
                          "for and which moves you more than the flowers do.";
        }
        const std::string& thanks = with_keepsake && !quest.thanks_keepsake.empty()
            ? quest.thanks_keepsake : quest.thanks;
        if (!thanks.empty()) plan.block += " Something like: " + in_quotes(thanks);
        return plan;
    }
    if (facts.accepted) {
        plan.block = "This person has already agreed to bring you " + things + " for " + pay +
                     ", and has not brought them yet.";
        if (facts.have > 0) {
            plan.block += " They have " + std::to_string(facts.have) + " on them, which is not all "
                          "of it; you want the " + std::to_string(quest.count) + " together, "
                          "though you say 'the lot' or 'all of them', never the number.";
        }
        plan.block += " Ask how it goes if you like, but you give nothing until they are in your hands.";
        return plan;
    }
    if (facts.locked) {
        plan.block = "You also want " + things + " brought to you, but one thing at a time; you "
                     "do not bring it up.";
        return plan;
    }
    plan.block = "You want " + things + " and cannot go for them yourself; nobody has agreed to "
                 "bring them yet (you never say the number aloud: a good bunch, enough for what "
                 "they are for). " +
                 (money ? "You would give " + pay + " to whoever does."
                        : quest.reward_text.empty()
                              ? "You have no money to give for it, only your thanks."
                              : "You have no money to give for it, but whoever does would have " +
                                    pay + ".") +
                 " Nobody you know has them; they are to be had about the town and the country, "
                 "and how this person comes by them is their affair. If they offer to, take them "
                 "up on it and " +
                 (money ? "say there is money in it, without naming a sum."
                        : quest.reward_text.empty() ? "be plain that thanks is all you have."
                                                    : "name what you can give.");
    if (!quest.offer.empty()) {
        plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
    }
    plan.question = "Did this person just agree, offer or promise to bring you " + things + "?";
    plan.on_yes = std::string(kQuestAccept);
    plan.direction = "[They have agreed to bring them. You are relieved, and you make sure they "
                     "know it is " + pay + " when the " + things + " are in your hands.]";
    return plan;
}

// A travel: the giver wants a look taken at the target and the news brought back. Nothing
// changes hands and the giver cannot check, so the three things Henry can say are rows the
// director writes with the giver's answer already decided: the truth (complete, paid) and
// silence (quiet, unpaid) once he has been within sight, and a lie (paid) at any time. The
// one looked at knows nothing, and the game's own Talk on them is untouched.
QuestPlan plan_travel(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    const std::string& who = quest.target_name;
    const std::string pay = reward_words(quest);
    const bool money = quest.pays_money();
    auto row = [&](std::string_view intent, const std::string& text, const std::string& reply,
                   std::string_view command, const std::string& plain_text,
                   const std::string& plain_reply) {
        plan.rows.push_back({std::string(intent), text.empty() ? plain_text : text,
                             reply.empty() ? plain_reply : reply, std::string(command)});
    };
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = "This person went and looked at " + who + " for you and came back and "
                         "told you what they saw, or would not; either way it is settled between "
                         "you, and you do not ask again.";
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person agreed to go and see how things stand with " + who +
                         " and come back and tell you, for " + pay + ". You have no way of knowing "
                         "whether they have been; you take their word for what they saw, and give "
                         "nothing until they tell you. Ask if they have been yet, if you like.";
            if (facts.seen) {
                row("tell", quest.tell, quest.thanks, kQuestComplete,
                    "I went. " + who + " is where you said, and going about the day.",
                    "Is that so. Well, that is something to know. Here: " + pay + ", as I said.");
            }
            row("lie", quest.lie, quest.lie_reply, kQuestLie,
                "I went. Things are bad with " + who + "; you were right to worry.",
                "I knew it. God help us. Here, " + pay + "; you have earned it.");
            if (facts.seen) {
                row("quiet", quest.quiet, quest.quiet_reply, kQuestQuiet,
                    "I went and I saw. What I saw is my business.",
                    "Then you have wasted your walk and my time. Nothing for nothing.");
            }
            return plan;
        }
        if (facts.locked) {
            plan.block = "You also want somebody to go and look at " + who +
                         " for you, but one thing at a time; you do not bring it up.";
            return plan;
        }
        plan.block = "You want to know how things stand with " + who + " and cannot go and look "
                     "yourself, or will not be seen to; nobody has agreed to go for you yet. " +
                     (money ? "You would give " + pay + " to whoever does."
                            : quest.reward_text.empty()
                                  ? "You have no money to give for it, only your thanks."
                                  : "You have no money to give for it, but whoever does would have " +
                                        pay + ".") +
                     " You do not want " + who + " spoken to, only looked at. If this person "
                     "offers to go, take them up on it, say where " + who + " is to be found and " +
                     (money ? "say there is money in it, without naming a sum." : "name what you can give.");
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to go and look at " + who +
                        " for you and bring you word?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to go. You tell them again where " + who +
                         " is to be found, that they are not to speak to " + who +
                         ", and that it is " + pay + " when they come back with word.]";
        return plan;
    }
    // The one looked at. Nobody has told them anything, and nobody will.
    plan.block = "You know nothing of anyone being sent to look at you, and nothing in this "
                 "conversation tells you.";
    return plan;
}

// What the giver has, put after "nobody has agreed yet": the money, the thing in kind, or
// thanks alone.
std::string offer_words(const Quest& quest) {
    const std::string pay = reward_words(quest);
    if (quest.pays_money()) return "You would give " + pay + " to whoever does.";
    if (quest.reward_text.empty()) return "You have no money to give for it, only your thanks.";
    return "You have no money to give for it, but whoever does would have " + pay + ".";
}

std::string name_it(const Quest& quest) {
    if (quest.pays_money()) return "say there is money in it, without naming a sum.";
    if (quest.reward_text.empty()) return "be plain that thanks is all you have.";
    return "name what you can give.";
}

// The rows the three thingless kinds share at the giver's once the errand is taken up. The
// giver cannot check any of it, so Henry's word is what pays: `report` is the truth when
// the store says the thing is done (complete, paid) and the same words are a lie when it
// does not (lie, paid the same); `confess` is the man dead (unpaid), when the store says so.
struct Rows {
    QuestPlan& plan;
    void add(std::string_view intent, const std::string& text, const std::string& reply,
             std::string_view command, const std::string& plain_text,
             const std::string& plain_reply) {
        plan.rows.push_back({std::string(intent), text.empty() ? plain_text : text,
                             reply.empty() ? plain_reply : reply, std::string(command)});
    }
};

// A beat: the giver has been wronged by a man and wants him made to answer for it and make
// it right -- an apology, a debt, a word taken back -- and the man has a menu of his own
// while the errand is open (the game's Talk on him: be talked round, frightened, or bought
// off), with the fists the player's last resort. The store knows which happened -- his word
// given (handed), knocked down (beaten), dead -- and the giver knows none of it until Henry
// says.
QuestPlan plan_beat(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    Rows rows{plan};
    const std::string& who = quest.target_name;
    const std::string pay = reward_words(quest);
    const bool money = quest.pays_money();
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = "This person dealt with " + who + " for you, or told you they had, and "
                         "that is settled between you; you do not go over it again.";
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person agreed to go and put it to " + who + " on your account, that "
                         "he is to make it right with you, for " + pay + ". You have no way of "
                         "knowing whether they have, or how it went; you take their word for it, "
                         "and give nothing until they tell you. Ask how it went if you like.";
            if (facts.dead) {
                plan.block += " If they tell you " + who + " is dead, that is far more than you "
                              "asked for and you want no part of it.";
                rows.add("confess", quest.confess, quest.dead_reply, kQuestConfess,
                         who + " is dead.",
                         "Dead? I asked for an apology, not a killing. Keep away from me, and "
                         "keep my name out of it.");
                return plan;
            }
            if (facts.handed) {
                rows.add("report", quest.report, quest.thanks, kQuestComplete,
                         "I have spoken to " + who + ". He has agreed to make it right with you.",
                         "Has he. Well. That is what I wanted. Here, " + pay + ", as I said.");
                return plan;
            }
            if (facts.beaten) {
                plan.block += " You did not ask for the man to be hurt; if they tell you he was, "
                              "you take it as you would, and pay, since he has answered for it "
                              "one way or another.";
                rows.add("fought", quest.fought, quest.fought_reply, kQuestComplete,
                         "He would not hear of it, so I knocked him down. " + who +
                         " will think twice before he crosses you again.",
                         "You did what? Well. He had it coming, I suppose, and he will not "
                         "forget it. Here, " + pay + ", and say nothing of this to anyone.");
                return plan;
            }
            rows.add("report", quest.report, quest.thanks, kQuestLie,
                     "I have spoken to " + who + ". He has agreed to make it right with you.",
                     "Has he. Well. That is what I wanted. Here, " + pay + ", as I said.");
            return plan;
        }
        if (facts.locked) {
            plan.block = "You also want " + who + " made to make things right with you, but one "
                         "thing at a time; you do not bring it up.";
            return plan;
        }
        plan.block = "You have been wronged by " + who + " and want him made to answer for it and "
                     "make it right, to say sorry, to pay what he owes, to take back what he said, "
                     "and cannot or will not go and put it to him yourself; nobody has agreed to "
                     "yet. " + offer_words(quest) +
                     " You are not asking for him to be beaten; you want him to make amends, and "
                     "how a stranger gets him to is the stranger's business. If this person offers "
                     "to go, take them up on it, say where " + who + " is to be found, what would "
                     "make it right, and " + name_it(quest);
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to go and make " + who +
                        " answer to you, or make it right with you?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to go. You tell them again where " + who +
                         " is to be found, what would make it right, and that it is " +
                         pay + " when he has agreed to it.]";
        return plan;
    }
    // The man himself. Whether he is talked round is the game's menu on him while the
    // errand is open, not this conversation; so this is before anyone has come for him, or
    // after.
    const std::string& owner = quest.giver_name;
    if (facts.dead) {
        plan.block.clear();
        return plan;
    }
    if (facts.beaten) {
        plan.block = "This person came to you on " + owner + "'s account, and when it came to it "
                     "they beat you. You want no more of them; you are short with them and keep "
                     "your distance.";
        return plan;
    }
    if (facts.handed || facts.done) {
        plan.block = "This person came to you on " + owner + "'s behalf and you agreed, one way or "
                     "another, to make things right with " + owner + ". You would sooner not go "
                     "over it.";
        return plan;
    }
    plan.block = "Between you and " + owner + " there is bad blood, and you hold that you did " +
                 owner + " no wrong worth the name. If it comes up, you give your own side and "
                 "make light of it; nothing said in passing is going to make you sorry.";
    return plan;
}

// A pray: the giver wants a prayer said for them at a place, and nobody else is party to
// it. The store knows whether Henry has stood at the place (the mod's Watch); the giver
// knows nothing until Henry says, and cannot tell a true report from a false one.
QuestPlan plan_pray(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    if (quest.role != QuestRole::Giver) return plan;
    Rows rows{plan};
    const std::string pay = reward_words(quest);
    const std::string where = quest.spot_text.empty() ? "the place you named" : quest.spot_text;
    if (facts.done) {
        plan.block = "This person went and said a prayer for you at " + where + ", or told you "
                     "they had, and that is settled between you; you are grateful, and you do "
                     "not ask again.";
        return plan;
    }
    if (facts.accepted) {
        plan.block = "This person agreed to go to " + where + " and pray there for you, for " +
                     pay + ". You have no way of knowing whether they have; you take their word "
                     "for it, and give nothing until they tell you. Ask if they have been yet, "
                     "if you like.";
        rows.add("report", quest.report, quest.thanks, facts.prayed ? kQuestComplete : kQuestLie,
                 "I went to " + where + " and said your prayer.",
                 "God bless you for it. Here, " + pay + ", as I said.");
        return plan;
    }
    if (facts.locked) {
        plan.block = "You also want a prayer said for you at " + where + ", but one thing at a "
                     "time; you do not bring it up.";
        return plan;
    }
    if (facts.cannot_pray) {
        // The game will not let this player kneel at a shrine yet, so the favour is not
        // asked of them: the giver has the want and holds it back, and finds nothing in
        // this person to make them ask.
        plan.block = "You want a prayer said for you at " + where + ", and cannot go yourself, "
                     "but this is not somebody you would trust with a prayer, they do not "
                     "strike you as a praying sort, so you keep it to yourself and do not ask.";
        return plan;
    }
    plan.block = "You want a prayer said for you at " + where + ", and cannot go yourself; "
                 "nobody has agreed to go for you yet. " + offer_words(quest) +
                 " If this person offers to, take them up on it, say plainly where it is and " +
                 name_it(quest);
    if (!quest.offer.empty()) {
        plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
    }
    plan.question = "Did this person just agree, offer or promise to go and pray at " + where +
                    " for you?";
    plan.on_yes = std::string(kQuestAccept);
    plan.direction = "[They have agreed to go. You tell them again where " + where +
                     " is, what they are to pray for, and that it is " + pay +
                     " when they come back.]";
    return plan;
}


// A leave: the giver wants a thing of theirs carried to a place and put down there, at an
// hour, for whatever they believe is owed it; something cries out in the dark after, and the
// cry is the news they wait for. Nothing comes back and the giver cannot check, so the rows
// are the travel's: the truth (complete, paid) and silence (quiet, unpaid) once the thing is
// down, and a lie (paid) at any time. A return is the same errand with the thing going back
// to a corpse it was taken from; the giver's words differ, the rows do not.
QuestPlan plan_spot(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    if (quest.role != QuestRole::Giver) return plan;
    const bool back = quest.is_return();
    const std::string pay = reward_words(quest);
    const bool money = quest.pays_money();
    const std::string thing = quest.item_name.empty() ? std::string(back ? "things" : "thing") : quest.item_name;
    const std::string where = quest.spot_text.empty() ? "the place you named" : quest.spot_text;
    const std::string errand = back ? "put your " + thing + " back where you took them from, at " + where
                                    : "carry your " + thing + " to " + where + " and leave it there in the dead of night";
    auto row = [&](std::string_view intent, const std::string& text, const std::string& reply,
                   std::string_view command, const std::string& plain_text,
                   const std::string& plain_reply) {
        plan.rows.push_back({std::string(intent), text.empty() ? plain_text : text,
                             reply.empty() ? plain_reply : reply, std::string(command)});
    };
    if (facts.done) {
        plan.block = "This person went to " + where + " for you and came back and told you how "
                     "it went, or would not; either way it is settled between you, and you do "
                     "not ask again.";
        return plan;
    }
    if (facts.accepted) {
        plan.block = "This person has your " + thing + " and agreed to " + errand + ", for " + pay +
                     ". You have no way of knowing whether they have been; you take their word "
                     "for what they did and what they heard, and give nothing until they tell "
                     "you. Ask if they have been yet, if you like.";
        if (facts.left) {
            row("tell", quest.tell, quest.thanks, kQuestComplete,
                back ? "It's done. They're back where they lay, by the coffin."
                     : "It's done. I left it there at the hour, and something cried out in the dark after.",
                back ? "God be thanked. Here: " + pay + ", as I said."
                     : "Then they took it. Here: " + pay + ", as I said, and not a word to anyone.");
        }
        row("lie", quest.lie, quest.lie_reply, kQuestLie,
            back ? "It's done. They're back where they lay." : "It's done. I left it there, and all was quiet.",
            back ? "God be thanked. Here, " + pay + ", and we'll say no more of it."
                 : "Quiet. Well. Quiet may be their way. Here, " + pay + "; you've earned it.");
        if (facts.left) {
            row("quiet", quest.quiet, quest.quiet_reply, kQuestQuiet,
                "I went. What I did there and what I heard is my business.",
                "Then you've had your walk and I've had nothing. Nothing for nothing.");
        }
        return plan;
    }
    if (facts.locked) {
        plan.block = "You also want somebody to " + errand + ", but one thing at a time; you do "
                     "not bring it up.";
        return plan;
    }
    plan.block = std::string("You want somebody to ") + errand + ", and " +
                 (back ? "will not go down there again yourself" : "cannot go yourself, or dare not") +
                 "; nobody has agreed to yet. " + offer_words(quest) +
                 " If this person offers, take them up on it, say plainly where it is" +
                 (back ? ", and the way down," : ", and that it must be at midnight,") + " and " +
                 name_it(quest);
    if (!quest.offer.empty()) {
        plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
    }
    plan.question = "Did this person just agree, offer or promise to take your " + thing + " to " +
                    where + " for you?";
    plan.on_yes = std::string(kQuestAccept);
    plan.direction = "[They have agreed to go. You give them the " + thing + ", tell them again where " +
                     where + " is" + (back ? "" : " and that it must be done at midnight") +
                     ", and that it is " + pay + " when they come back with word.]";
    return plan;
}

// A mind: the giver wants somebody looked in on, more than once, hours apart. The visits are
// the game's own Talk on the one looked in on (the mod counts them and keeps the clock);
// the giver knows nothing until Henry says.
QuestPlan plan_mind(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    Rows rows{plan};
    const std::string& who = quest.target_name;
    const std::string pay = reward_words(quest);
    const int want = std::max(1, quest.count);
    const std::string visits = std::to_string(want) + (want == 1 ? " visit" : " visits");
    const std::string times = want == 1 ? "once" : want == 2 ? "twice" : std::to_string(want) + " times";
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = "This person looked in on " + who + " for you, or told you they had, and "
                         "that is settled between you; you are grateful, and you do not ask again.";
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person agreed to look in on " + who + " for you, " + times +
                         ", some hours apart, and tell you how " + who + " is keeping, for " + pay +
                         ". You have no way of knowing whether they have; you take their word for "
                         "it, and give nothing until they tell you. Ask how " + who + " is, if you like.";
            if (facts.dead) {
                plan.block += " If they tell you " + who + " is dead, it is the thing you were afraid of.";
                rows.add("confess", quest.confess, quest.dead_reply, kQuestConfess,
                         who + " is dead. I am sorry.",
                         "Dead. God rest him. I knew it, I knew it. Go on, leave me be.");
                return plan;
            }
            const bool minded = facts.checks >= want;
            if (!minded && facts.checks > 0) {
                plan.block += " So far they say they have been " +
                              (facts.checks == 1 ? std::string("once") : std::to_string(facts.checks) + " times") +
                              ", which is not all of it.";
            }
            rows.add("report", quest.report, quest.thanks, minded ? kQuestComplete : kQuestLie,
                     "I have looked in on " + who + " " + times + ", as you asked. Keeping well enough.",
                     "Thank God for that. You have taken a weight off me. Here, " + pay + ", as I said.");
            return plan;
        }
        if (facts.locked) {
            plan.block = "You also want somebody to look in on " + who + " for you, but one thing at "
                         "a time; you do not bring it up.";
            return plan;
        }
        plan.block = "You are worried for " + who + " and want somebody to look in on him " + times +
                     ", some hours apart, and tell you how he is keeping; you cannot keep doing it "
                     "yourself. Nobody has agreed to yet. " + offer_words(quest) +
                     " If this person offers to, take them up on it, say where " + who +
                     " is to be found and " + name_it(quest);
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to look in on " + who +
                        " for you and tell you how he is?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to. You tell them again where " + who +
                         " is to be found, that it is " + visits + " you want, hours apart, and that "
                         "it is " + pay + " when they come back with word.]";
        return plan;
    }
    // The one looked in on. The visits themselves are the game's Talk on them, not this.
    const std::string& owner = quest.giver_name;
    if (facts.dead) {
        plan.block.clear();
        return plan;
    }
    if (facts.checks > 0) {
        plan.block = "This person has looked in on you " +
                     (facts.checks == 1 ? std::string("once") : std::to_string(facts.checks) + " times") +
                     " on " + owner + "'s account, asking how you keep. You are touched that " + owner +
                     " troubles, and a little tired of being fussed over.";
        return plan;
    }
    plan.block = "You know nothing of anyone being sent to look in on you, and nothing in this "
                 "conversation tells you.";
    return plan;
}

// An ask: the giver believes a tale the one who knows has told everyone, and wants the truth
// of it; the one who knows has a menu of his own while the errand is open (the game's Talk on
// him: be talked round, frightened, or paid), and the truth is what the winning branch gives
// up (handed). The giver knows none of it until Henry says, and cannot tell the truth from a
// tale -- so the rows are a travel's: the truth (complete, paid) and silence (quiet, unpaid)
// once it has been told, a lie (paid) at any time, and the man dead (confess, unpaid).
QuestPlan plan_ask(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    Rows rows{plan};
    const std::string& who = quest.target_name;
    const std::string pay = reward_words(quest);
    // The word carried on (Quest::carries_on): the truth told, the giver pays and sends Henry
    // to the onward soul with it, and the errand ends there, on that soul's own menu. So the
    // tell row's command is `onward` rather than `complete`, and the giver has one more state
    // -- told and waiting to hear it has been said -- which opens on `greet_onward`.
    const bool onward = quest.carries_on();
    const std::string& to = quest.onward_name;
    if (quest.role == QuestRole::Onward) {
        // The one sent to. They hear it in the game's own Talk on them, not in this
        // conversation, and know nothing of it before.
        const std::string& from = quest.giver_name;
        if (facts.done) {
            plan.block = "This person came to you with word from " + from + " of what " + who +
                         " had been keeping back, which you have heard and will deal with as you "
                         "see fit; you do not chew it over with them.";
            return plan;
        }
        plan.block = "You expect nothing from " + from + " and know of no word from him. If this "
                     "person says they have something to tell you from him, tell them to say it "
                     "plainly, and no more; you are not one for talk with a stranger.";
        return plan;
    }
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = onward
                ? "This person went to " + who + " for you, came back and told you what they got "
                  "out of them, and carried it on to " + to + " as you asked, or told you "
                  "something else, or would not say; either way it is settled between you, and "
                  "you do not ask again."
                : "This person went to " + who + " for you and came back and told you what "
                  "they got out of them, or would not; either way it is settled between "
                  "you, and you do not ask again.";
            return plan;
        }
        if (onward && facts.onward) {
            plan.block = "This person got the truth out of " + who + " and told it you. You have "
                         "given them what you promised for it, " + pay + ", and asked one "
                         "thing more: to carry what they learned to " + to + ", since you will not "
                         "go yourself. You have no way of knowing whether it has been said yet; ask "
                         "whether they have been to " + to + ", and say again where " + to +
                         " is to be found if they seem unsure. Nothing more is owed, and you do "
                         "not pay twice.";
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person agreed to go to " + who + " and get the truth of the matter "
                         "out of them for you, for " + pay + ". You have no way of knowing whether "
                         "they have been, or what they were told; you take their word for it, and "
                         "give nothing until they tell you. Ask if they have been yet, if you like.";
            if (facts.dead) {
                plan.block += " If they tell you " + who + " is dead, the truth has gone with them, "
                              "and that is far worse than not knowing.";
                rows.add("confess", quest.confess, quest.dead_reply, kQuestConfess,
                         who + " is dead. Whatever they knew went with them.",
                         "Dead? Then I will never know. God forgive me for asking. Leave me be.");
                return plan;
            }
            if (onward) {
                plan.block += " When they tell you the truth, you pay, and in the same breath ask "
                              "them to carry it to " + to + ", since you will not go yourself; "
                              "that is in your written answer.";
            }
            if (facts.handed) {
                rows.add("tell", quest.tell, quest.thanks, onward ? kQuestOnward : kQuestComplete,
                         "I got it out of " + who + ". Here is the truth of it.",
                         onward ? "So that is how it was. Here, " + pay + ", as I said. One thing more: "
                                  "carry it to " + to + ", for I will not go myself."
                                : "So that is how it was. Well. I would rather know. Here, " + pay + ", as I said.");
            }
            rows.add("lie", quest.lie, quest.lie_reply, kQuestLie,
                     "I spoke to " + who + ". It is as they told you; there is no more to it.",
                     "Then I have been fretting over nothing. Here, " + pay + "; you have earned it.");
            if (facts.handed) {
                rows.add("quiet", quest.quiet, quest.quiet_reply, kQuestQuiet,
                         who + " told me. What I was told is going no further.",
                         "Then you have wasted your walk and my time. Nothing for nothing.");
            }
            return plan;
        }
        if (facts.locked) {
            plan.block = "You also want the truth of a matter got out of " + who +
                         ", but one thing at a time; you do not bring it up.";
            return plan;
        }
        plan.block = "There is a thing " + who + " has told you, and told everyone, and you do not "
                     "believe it is the whole of it; you want the truth of the matter got out of " +
                     who + ", and cannot or will not press for it yourself. Nobody has agreed to "
                     "yet. " + offer_words(quest) +
                     " You know only the tale you were told; do not invent what the truth might be. "
                     "If this person offers to go, take them up on it, say where " + who +
                     " is to be found, what you were told and why it does not sit right, and " +
                     name_it(quest);
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to go to " + who +
                        " and get the truth of it for you?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to go. You tell them again where " + who +
                         " is to be found, what you were told, and that it is " + pay +
                         " when they come back with the truth.]";
        return plan;
    }
    // The one who knows. Whether they give the truth up is the game's menu on them while the
    // errand is open, not this conversation; so this is before anyone has come asking on the
    // giver's account, or after.
    const std::string& owner = quest.giver_name;
    if (facts.dead) {
        plan.block.clear();
        return plan;
    }
    if (facts.handed || facts.done) {
        plan.block = "This person came to you on " + owner + "'s account and got the truth out of "
                     "you, one way or another: the thing you had told " + owner +
                     " and everyone else was not the whole of it. It is said now, and you would "
                     "sooner not go over it again.";
        return plan;
    }
    plan.block = "There is a matter " + owner + " has asked you about, and what you told " + owner +
                 ", and everyone else, is not the whole truth of it. You keep to the tale you "
                 "told; if this person brings it up, you give them the same tale, and nothing said "
                 "in passing is going to get the rest out of you.";
    return plan;
}

// An ambush (quest-design/set-aside-stories.md section 5): the giver believes themselves
// cursed and wants the one who can lift it found; the target, a drinker, is said to know the
// way, and has a menu of his own while the errand is open (the game's Talk on him: the false
// way written down, then the true way once Henry has been set on at the place and come
// back). The true way given is `handed`, as an ask's truth is, and the giver's rows are an
// ask's: the name carried back (complete, paid) and silence (quiet, unpaid) once it has
// been given, a lie (paid) at any time, and the drinker dead (confess, unpaid).
QuestPlan plan_ambush(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    Rows rows{plan};
    const std::string& who = quest.target_name;
    const std::string pay = reward_words(quest);
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = "This person went to " + who + " for you and came back with the name of "
                         "one who can lift what is on you, or would not say; either way it is "
                         "settled between you, and you do not ask again.";
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person agreed to go to " + who + " at the Hole in the Wall and get "
                         "out of them the way to one who can lift the curse you believe is on you, "
                         "for " + pay + ". You have no way of knowing whether they have been, or "
                         "what they were told; you take their word for it, and give nothing until "
                         "they tell you. Ask if they have been yet, if you like.";
            if (facts.dead) {
                plan.block += " If they tell you " + who + " is dead, the way has gone with them.";
                rows.add("confess", quest.confess, quest.dead_reply, kQuestConfess,
                         who + " is dead. Whatever they knew of your curse-lifter went with them.",
                         "Dead? Then there is no help for me. Leave me be.");
                return plan;
            }
            if (facts.handed) {
                rows.add("tell", quest.tell, quest.thanks, kQuestComplete,
                         "I got it out of " + who + ". I know who it is now, and where.",
                         "Then there is hope yet. Here, " + pay + ", as I said.");
            }
            rows.add("lie", quest.lie, quest.lie_reply, kQuestLie,
                     "I spoke to " + who + ". There is no such person; it was drink talking.",
                     "No one? Then God help me. Here, " + pay + "; you went, at least.");
            if (facts.handed) {
                rows.add("quiet", quest.quiet, quest.quiet_reply, kQuestQuiet,
                         who + " told me. I am keeping it to myself.",
                         "Keeping it? Then you have wasted your walk and my hope. Nothing for nothing.");
            }
            return plan;
        }
        if (facts.locked) {
            plan.block = "You also want the way to one who lifts curses got out of " + who +
                         ", but one thing at a time; you do not bring it up.";
            return plan;
        }
        plan.block = "You believe a curse is on you, and have heard that " + who + ", who drinks "
                     "at the Hole in the Wall, knows of one who can lift it; you will not be seen in "
                     "that house yourself. Nobody has agreed to go yet. " + offer_words(quest) +
                     " You know only that " + who + " is said to know; do not invent who the "
                     "curse-lifter might be. If this person offers to go, take them up on it, say "
                     "where " + who + " is to be found, what has befallen you, and " + name_it(quest);
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to go to " + who +
                        " and find out the way to one who can lift the curse?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to go. You tell them again where " + who +
                         " is to be found, what has befallen you, and that it is " + pay +
                         " when they come back with the name.]";
        return plan;
    }
    // The drinker. The two ways are the game's menu on him while the errand is open, not this
    // conversation; so this is before anyone has come asking on the giver's account, or after.
    const std::string& owner = quest.giver_name;
    if (facts.dead) {
        plan.block.clear();
        return plan;
    }
    if (facts.handed || facts.done) {
        plan.block = "This person came to you on " + owner + "'s account for the way to one who "
                     "lifts curses; you sent them wrong the first time, to where friends of yours "
                     "were waiting, and when they came back on their feet you gave them the true "
                     "way. You are frightened of them and would sooner not go over it.";
        return plan;
    }
    if (facts.directed) {
        plan.block = "This person came to you on " + owner + "'s account for the way to one who "
                     "lifts curses, and you wrote them a way down, not the true one. You know "
                     "where it sends them and who waits there; you give nothing of that away, and "
                     "if they ask again you tell them to go where the paper says.";
        return plan;
    }
    plan.block = "You know of one who lifts curses, and you know the way to them; " + owner +
                 " may have heard as much. You say nothing of it unasked, and nothing said in "
                 "passing gets it out of you.";
    return plan;
}

// A buy (section 5): the giver knows now who can lift the curse and sends Henry with the
// price -- a garment of their own and a sum, both given over when Henry agrees -- and the
// seller has a menu of his own while the errand is open (the game's Talk on him: the terms,
// and the charm handed over for both). The charm in Henry's hands is `handed` (or `have`),
// and the giver's rows are the truth (complete, paid) and a lie about the price (paid, and
// the difference pocketed). Nothing is paid without the charm.
QuestPlan plan_buy(const Quest& quest, const QuestFacts& facts) {
    QuestPlan plan;
    Rows rows{plan};
    const std::string& who = quest.target_name;
    const std::string pay = reward_words(quest);
    const std::string& thing = quest.item_name;
    const bool carrying = facts.handed || facts.have > 0;
    if (quest.role == QuestRole::Giver) {
        if (facts.done) {
            plan.block = "This person went to " + who + " for you with your hood and your money "
                         "and brought you back the " + thing + "; it is settled between you, and "
                         "whether it works is between you and God.";
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person agreed to take your hood and your money to " + who +
                         " and bring back the " + thing + ", for " + pay + ". You have no way of "
                         "knowing what " + who + " asked of them; you give nothing until the " +
                         thing + " is in your hand.";
            if (facts.dead) {
                plan.block += " If they tell you " + who + " is dead, there is no " + thing +
                              " to be had, and your hood and money are gone with the errand.";
            }
            if (carrying) {
                plan.block += " They have it with them now.";
                rows.add("tell", quest.tell, quest.thanks, kQuestComplete,
                         "Here is your " + thing + ". " + who + " took the hood and the money, as you said.",
                         "Give it here. Here, " + pay + ", as I said. Now we shall see.");
                rows.add("lie", quest.lie, quest.lie_reply, kQuestLie,
                         "Here is your " + thing + ". " + who + " wanted twice the money; I made up the rest.",
                         "Twice? Thief. Here, then, what you are out of pocket, and " + pay + " besides.");
            }
            return plan;
        }
        if (facts.locked) {
            plan.block = "You also have a " + thing + " to send for from " + who +
                         ", but one thing at a time; you do not bring it up.";
            return plan;
        }
        plan.block = "You know now that " + who + " can lift the curse you believe is on you, and "
                     "will not go yourself; the price is a garment of yours off your own head and "
                     "a sum of money, and you want a stranger to carry both and bring back the " +
                     thing + ". Nobody has agreed to yet. " + offer_words(quest) +
                     " If this person offers to go, take them up on it, say where " + who +
                     " is to be found, what " + who + " asks, and " + name_it(quest);
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to take your hood and your "
                        "money to " + who + " and bring back the " + thing + "?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to go. You give them the hood and the money, tell them "
                         "again where " + who + " is to be found, and that it is " + pay +
                         " when they come back with the " + thing + ".]";
        return plan;
    }
    // The seller. The terms are the game's menu on him while the errand is open, not this
    // conversation.
    const std::string& owner = quest.giver_name;
    if (facts.dead) {
        plan.block.clear();
        return plan;
    }
    if (facts.handed || facts.done) {
        plan.block = "This person came to you on " + owner + "'s account and bought a " + thing +
                     " off you for " + owner + "'s own hood and your price in money. The bargain "
                     "is made and you do not go over your terms again.";
        return plan;
    }
    plan.block = "You make charms against curses, for a price: a garment the sufferer has worn, "
                 "and money. You say nothing of " + owner + " unasked, and nothing said in passing "
                 "moves your price.";
    return plan;
}

}  // namespace

bool how_matches(std::string_view how, std::string_view wanted) {
    if (wanted.empty()) return true;
    while (!wanted.empty()) {
        const std::size_t bar = wanted.find('|');
        const std::string_view token = wanted.substr(0, bar);
        wanted = bar == std::string_view::npos ? std::string_view() : wanted.substr(bar + 1);
        if (token == how) return true;
        if (token == "truth" && !untrue_how(how)) return true;
    }
    return false;
}

bool untrue_how(std::string_view how) {
    return how == "lied" || how == "quiet" || how == "kept" || how == "dead" || how == "pinned" ||
           how == "covered" || how == "fibbed" || how == "evaded" || how == "caught" || how == "abandoned";
}

std::string dictation_intent(int part, int outcome) {
    const char* word = outcome == 1 ? "reconsider" : outcome == 2 ? "soften" : outcome == 3 ? "embellish" : "write";
    return std::string(word) + ":" + std::to_string(part);
}

std::pair<int, int> dictation_part(std::string_view intent) {
    const std::size_t colon = intent.find(':');
    if (colon == std::string_view::npos || colon + 1 >= intent.size()) return {0, -1};
    const std::string_view word = intent.substr(0, colon);
    const int outcome = word == "write" ? 0 : word == "reconsider" ? 1 : word == "soften" ? 2
                        : word == "embellish" ? 3 : -1;
    if (outcome < 0) return {0, -1};
    int part = 0;
    for (const char c : intent.substr(colon + 1)) {
        if (c < '0' || c > '9') return {0, -1};
        part = part * 10 + (c - '0');
    }
    if (part < 1 || part > Quest::Dictation::kParts) return {0, -1};
    return {part, outcome};
}

bool giver_relents(const Quest& quest, const Approach& approach, int met) {
    if (met >= 3) return true;
    if (approach.charisma == kUnread) return true;
    return approach.charisma >= relent_threshold(quest.difficulty);
}

namespace {

// The giver's own opening for the visit the plan describes, when one was written for it.
// Decided off the plan rather than the facts, so it cannot disagree with the paragraph: a
// settling turn (the plan owes the completion) opens on the thanks the writer gave the
// giver, or on the line for a thing brought unasked when nobody ever agreed; a visit with
// the favour taken up and nothing settled opens on the owed line; a first meeting with the
// errand there to be offered opens on the giver's own line for it (`greet_offer`), when the
// writer gave one. Anything else is an ordinary visit and the state greeting serves. `met`
// is Standing::met: the conversations counting this one, 0 or 1 a first.
std::string visit_greeting(const Quest& quest, const QuestFacts& facts, const QuestPlan& plan, int met) {
    if (quest.role != QuestRole::Giver) return {};
    // Back from an unyielding holder with his refusal: the giver's line sending Henry to
    // the chest and naming the one who keeps the house, the first time (the naming rides on
    // it); after that, the owed line serves as for any open errand.
    if (quest.has_helper() && facts.rebuffed && !facts.named && facts.accepted && !facts.done &&
        facts.have <= 0) {
        return quest.greet_rebuffed;
    }
    // Back from a holder with a secret, refused and with nothing yet seen: the giver's line
    // sending Henry after his nights, when one was written.
    if (quest.has_expose() && facts.rebuffed && !facts.seen && facts.accepted && !facts.done &&
        facts.have <= 0 && !quest.greet_rebuffed.empty()) {
        return quest.greet_rebuffed;
    }
    if (!plan.opening_commands.empty()) {
        if (!facts.accepted) return quest.greet_settle_unasked;
        // A find's keepsake brought back with the things: the giver's thanks for that.
        if (quest.has_keepsake() && facts.keepsake && !quest.thanks_keepsake.empty()) {
            return quest.thanks_keepsake;
        }
        // The thing won by the hold Henry found over the holder: the thanks for that.
        return quest.thanks_for(facts.how);
    }
    // An ask's giver told and waiting to hear the word has been carried on: their own line
    // for it when the writer gave one, else the owed line serves.
    if (quest.carries_on() && facts.onward && !facts.done) {
        return quest.greet_onward.empty() ? quest.greet_owed : quest.greet_onward;
    }
    // A letter delivered and the writer waiting to hear what was said: his own line for it
    // when one was written, else the owed line serves.
    if (quest.is_deliver() && facts.handed && !facts.done && !quest.greet_handed.empty()) return quest.greet_handed;
    // Back with a dictated letter still unwritten: the giver's line for sitting Henry down
    // again, where one was written.
    if (quest.has_dictation() && facts.accepted && !facts.handed && !facts.done && facts.wrote.empty() &&
        !quest.dictation.greet_unwritten.empty()) {
        return quest.dictation.greet_unwritten;
    }
    if (facts.accepted && !facts.done && !facts.locked) return quest.greet_owed;
    // The errand offered on a first meeting: the giver who hailed Henry over says why
    // before Henry asks (Benesh, notes.md 13 Oct 2026). Only the once -- back a second time
    // with it still unasked, the soul's own greeting serves, and the opening row still leads
    // to the offer.
    if (!plan.question.empty() && !facts.accepted && met <= 1) return quest.greet_offer;
    return {};
}

QuestPlan plan_kind(const Quest& quest, const QuestFacts& facts, const Approach& approach, int met);

}  // namespace

QuestPlan plan_quest(const Quest& quest, const QuestFacts& facts, const Approach& approach,
                     int met) {
    QuestPlan plan = plan_kind(quest, facts, approach, met);
    // Done, the errand has left the soul somewhere the rest of their sheet (written before
    // it) does not know about: the bell-ringer cleared, the priest's cup back. The written
    // line for the ending, or the plain one, stands over the sheet (notes.md 14 Oct 2026).
    if (quest.exists() && !facts.voided && facts.done && facts.how != "abandoned") {
        const std::string since = quest.since_line(facts.how);
        if (!since.empty()) {
            if (!plan.block.empty()) plan.block += " ";
            plan.block += "Since then, and over anything else you have been told of your situation: " + since;
        }
    }
    if (quest.role == QuestRole::Giver && quest.exists() && !facts.voided) {
        // Given up to the giver's face (the give-up row, kQuestAbandon): over between them,
        // whatever the kind's plan would have made of a done errand.
        if (facts.done && facts.how == "abandoned") {
            QuestPlan over;
            over.block = "This person took on what you asked of them, and later came back only to tell "
                         "you they were giving it up; they would not do it. That is over between you: "
                         "you do not ask it of them again, you do not offer it, and you are short with "
                         "them about it, though not unkind.";
            return over;
        }
        // The errand open and nothing being settled on this very turn: Henry can give it up,
        // on a row that is on every list (notes.md 13 Oct 2026). Not once the other party is
        // dead: the confession is the only honest end of that, and the row for it is laid.
        const bool settling = std::find(plan.opening_commands.begin(), plan.opening_commands.end(),
                                        std::string(kQuestComplete)) != plan.opening_commands.end() ||
                              std::any_of(plan.rows.begin(), plan.rows.end(),
                                          [](const DirectorRow& row) { return row.command == kQuestComplete; });
        if (facts.accepted && !facts.done && !facts.locked && !facts.dead && !settling) {
            plan.rows.push_back({"give_up", std::string(kGiveUpRow), quest.give_up_reply, std::string(kQuestAbandon)});
        }
        // The giver's carrying (Quest::fast_travel_row; notes.md 25 Oct 2026): while the thing
        // is still to be got and nothing is being settled, between the hours written for it
        // (not with the hour unread: a carrying into the night would skip the chance it is
        // for). Before the yes the rows wait for it (after_yes) and the yes leaves the
        // conversation open for them; after it, the carrying alone is on every list, the
        // player having other rows to leave by. Both close the conversation.
        const bool open = !facts.done && !facts.locked && !facts.dead && !settling && !facts.handed &&
                          !facts.denounced && facts.have <= 0 && (facts.accepted || !plan.question.empty());
        if (quest.has_fast_travel() && open && approach.hour != kUnread &&
            approach.hour >= quest.fast_travel_from && approach.hour < quest.fast_travel_to) {
            DirectorRow go{"fast_travel", quest.fast_travel_row, quest.fast_travel_reply, std::string(kQuestFastTravel)};
            go.after_yes = !facts.accepted;
            go.closes = true;
            go.group = "fast_travel";
            plan.rows.push_back(std::move(go));
            if (!facts.accepted) {
                DirectorRow later{"fast_travel_later", quest.fast_travel_later_row, quest.fast_travel_later_reply, {}};
                later.after_yes = true;
                later.closes = true;
                later.group = "fast_travel";
                plan.rows.push_back(std::move(later));
            }
        }
    }
    // Every kind's offer tells the giver to "say where <who> is to be found", and a model
    // with nothing to go on says something: Anna put Matilda "a few doors down" when she is
    // on another street (notes.md, 8 Oct 2026). The way is the map marker's once Henry
    // has agreed, and the written `agreed` line carries the measured bearing where there
    // is one; what the giver says of it unprompted is a word of direction and no more.
    if (quest.role == QuestRole::Giver && plan.block.find(" is to be found") != std::string::npos) {
        plan.block += " Of where they are to go, say no more than a word of direction, the street, "
                      "or which way out of town; their map will mark the way once they have agreed. "
                      "You do not know it in paces or doors, so do not invent them.";
    }
    plan.greeting = visit_greeting(quest, facts, plan, met);
    return plan;
}

namespace {

QuestPlan plan_kind(const Quest& quest, const QuestFacts& facts, const Approach& approach,
                    int met) {
    // The approach decides one thing here: whether a dispute's giver gives his word when
    // the player carries the holder's condition to him. The game weighs the holder itself,
    // and the rest of the giver's side turns on what the player carries and what they say.
    QuestPlan plan;
    if (!quest.exists() || quest.role == QuestRole::None) return plan;
    // A scene the story went past: the soul never has this favour, and nothing says so.
    if (facts.voided) return plan;
    // A scene not yet reached, from the other party's side: nobody has come to them on
    // anyone's account, and the sheet does not say they are keeping something back (Benesh,
    // lied to over the chalice, was told the priest had asked about him; notes.md 9 Oct 2026).
    if (facts.locked && quest.role != QuestRole::Giver) return plan;
    if (quest.is_deliver()) return plan_deliver(quest, facts);
    if (quest.is_find()) return plan_find(quest, facts);
    if (quest.is_travel()) return plan_travel(quest, facts);
    if (quest.is_beat()) return plan_beat(quest, facts);
    if (quest.is_pray()) return plan_pray(quest, facts);
    if (quest.is_spot()) return plan_spot(quest, facts);
    if (quest.is_mind()) return plan_mind(quest, facts);
    if (quest.is_ask()) return plan_ask(quest, facts);
    if (quest.is_ambush()) return plan_ambush(quest, facts);
    if (quest.is_buy()) return plan_buy(quest, facts);

    const std::string item = "your " + quest.item_name;
    const std::string pay = reward_words(quest);
    const bool money = quest.pays_money();
    const bool carrying = facts.have > 0;

    if (quest.role == QuestRole::Giver) {
        const std::string& holder = quest.target_name;
        // A chain's second link: the same person did the first, and both of you know it. A
        // story's later scene is a link too, but its scene before may be anybody's and any
        // kind, and the circle's own words carry what came before; so only a chain's.
        const std::string before = quest.after_quest_id.empty() || quest.chain_id.empty()
            ? std::string()
            : " They already got your " + quest.after_item_name + " back for you, which is why "
              "you would ask them this.";
        if (facts.done) {
            if (facts.how == "denounced") {
                // Henry found the holder out and chose to denounce him rather than trade
                // (the mod's bedside, notes.md 22 Oct 2026): nothing came back, and the giver
                // was told so. The hold the holder had is gone with his good name; the giver
                // is safe without the thing, and knows it.
                plan.block = "This person went after " + item + " and came back without it: they found " +
                             holder + " out in something shameful, and rather than trade on it they told the "
                             "parish. You never got the thing back and gave nothing, but whatever " + holder +
                             " meant to do with it is finished, since nobody will take his word now. You are "
                             "safe, and you know who to thank for it, even if it was not the way you asked.";
                return plan;
            }
            if (facts.how == "lied") {
                // Told the holder never had it, and believed (the lie_have row): nothing came
                // back and nothing was paid, and the giver has given the thing up for lost.
                plan.block = "This person went to " + holder + " for " + item + " and came back saying he "
                             "never had it. You believed them, and gave nothing; the thing is lost "
                             "to you, and you do not hold it against them.";
                return plan;
            }
            plan.block = "This person got " + item + " back from " + holder + " for you and " +
                         (money ? "you paid them " + pay + " for it."
                                : "you gave them " + pay + " for it.") +
                         " That is settled between you, and you think the better of them for it.";
            return plan;
        }
        if (carrying && facts.accepted && quest.has_lie_have()) {
            // The thing in Henry's coat and a lie written for it (notes.md 9 Oct 2026, In the
            // Stocks): the giver does not see it, so the hand-in is a row rather than the
            // first turn's doing -- `give`, answered with the thanks and paid -- and beside it
            // the lie that the holder never had it, said as a lie on the row and weighed as a
            // Speech check against the giver. Won, the errand ends there, unpaid, with the
            // thing kept (the journal's lied log); lost, the giver says so and the errand
            // stands, with both rows still on the list.
            plan.block = "This person has already agreed to get " + item + " back from " + holder +
                         " for " + pay + ", and has not brought it yet, as far as you know. Ask how "
                         "it goes if you like, but you give nothing until it is in your hands.";
            // The thanks for the way the thing came (thanks_exposed where the holder's
            // silence bought more than the thing), else the plain ones.
            const std::string& thanks = quest.thanks_for(facts.how);
            if (&thanks == &quest.thanks_exposed) {
                plan.block += " Whatever hold they found over " + holder + " has bought you more than the "
                              "thing itself, as your written thanks say; you do not ask what the hold was.";
            }
            plan.rows.push_back({"give",
                                 quest.give.empty() ? "Here is your " + quest.item_name + "." : quest.give,
                                 thanks.empty() ? "You have it! Thank you. Here is what I promised: " + pay + "."
                                                : thanks,
                                 std::string(kQuestComplete)});
            plan.rows.push_back({"lie_have", "(Lie) " + quest.lie_have, quest.lie_have_reply, std::string(kQuestLie)});
            DirectorRow& lie = plan.rows.back();
            lie.check = "speech";
            lie.check_factor = check_factor(quest.difficulty);
            lie.fail_reply = quest.lie_have_caught.empty()
                ? "Never had it? Then what is that under your coat? Give it here, or get out of my sight."
                : quest.lie_have_caught;
            return plan;
        }
        if (carrying) {
            // Seen in their hands, so it is paid for on the first turn, before a word. The
            // graph wants the quest accepted before it can be done; a player who went to the
            // holder first never said yes to anybody, so the app says it for them.
            if (!facts.accepted) plan.opening_commands.emplace_back(kQuestAccept);
            plan.opening_commands.emplace_back(kQuestComplete);
            plan.block = "This person has brought " + item + " back from " + holder +
                         (facts.accepted ? ", as they agreed to."
                                         : "; they must have got it off him unasked.") +
                         " Thank them, in your own way, and " +
                         (money ? "pay them the " + pay +
                                      (facts.accepted ? " you promised." : " it is worth to you.")
                                : "give them what you said you would: " + pay + ".");
            if (quest.dispute && facts.apologised) {
                plan.block += " It cost you your pride with " + holder +
                              ", and you would sooner that were not mentioned.";
            }
            const std::string& thanks = quest.thanks_for(facts.how);
            if (&thanks == &quest.thanks_exposed) {
                plan.block += " Whatever hold they found over " + holder + " has bought you more than the "
                              "thing itself, as your written thanks say; you do not ask what the hold was.";
            }
            if (!thanks.empty()) {
                plan.block += " Something like: " + in_quotes(thanks);
            }
            return plan;
        }
        if (facts.accepted) {
            plan.block = "This person has already agreed to get " + item + " back from " +
                         holder + " for " + pay +
                         ", and has not brought it yet. Ask how it goes if you like, but you "
                         "give nothing until it is in your hands.";
            if (quest.has_expose() && facts.denounced) {
                // Back from the bedside with nothing (the mod's denounce row, notes.md 22 Oct
                // 2026): Henry chose to shame the holder rather than trade on what he saw, and
                // the giver has only his report to hear. The row is the telling (told_denounced,
                // answered with told_reply_denounced; plain words where none were written) and
                // completes the errand as `denounced`, the how the mod settled at the row:
                // nothing comes back and nothing is paid.
                const std::string& she = quest.seen_with_name;
                std::string told, told_reply;
                for (const Quest::Told& t : quest.told_by_how) {
                    if (t.how != "denounced") continue;
                    told = t.row;
                    told_reply = t.reply;
                }
                plan.block = "This person went after " + item + " for you and has come back without it. They "
                             "found " + holder + " out in the night where he should never have been, in " + she +
                             "'s bed, and rather than trade their silence for the thing they mean to tell the "
                             "parish, or have already. You do not know this until they tell you. Hear it: the "
                             "paper is not coming back, and you must decide what you make of that. Whatever " +
                             holder + " meant to do with it is finished, since nobody will take the word of a "
                             "priest found in a woman's bed, and you are safe without it; but it was not what "
                             "you asked for, and you have nothing to give for it.";
                plan.rows.push_back({"report_denounced",
                                     told.empty() ? "I found " + holder + " where he should not be, and I am not "
                                                    "keeping it quiet. Your paper stays with him."
                                                  : told,
                                     told_reply.empty()
                                         ? "Then it stays with him, and let him try to use it now. God forgive us both."
                                         : told_reply,
                                     std::string(kQuestComplete)});
                return plan;
            }
            if (quest.has_helper() && facts.rebuffed) {
                // The holder would not hear of it (the store's n; In the Stocks' sacristan,
                // notes.md 13 Oct 2026). The giver does not take no for an answer: the
                // thing is to come out of the holder's chest anyway, and he names the one
                // who keeps the house. Said on the opening line the first time (greet_
                // rebuffed), and the naming rides on Henry's row taking it up (accept_again,
                // kQuestNamed) for the mod to open her menu and mark her -- a yes to give,
                // rather than the ask settling itself on the opening (notes.md 13 Oct 2026);
                // after that it is a thing both of you know.
                const std::string& she = quest.helper_name;
                plan.block += " They have been to " + holder + " about it, and " + holder +
                              " took offence and would not hear of it; you expected no better. "
                              "You want " + item + " out of his chest regardless, and you " +
                              (facts.named ? "have told them so, and named " : "tell them so, and name ") +
                              she + ", the servant girl who keeps the house he lodges in and "
                              "is about outside it by day: a word from her would get them in, "
                              "and she has her price. You do not say the word steal, and you "
                              "do not want to hear how it is done, nor whether " + she +
                              " has been any help; that is between them and her.";
                if (!facts.named) {
                    plan.block += " You have just put it to them again, and wait on their answer.";
                    plan.rows.push_back({"accept_again",
                                         quest.named_row.empty() ? "His chest, then. I'll find this " + she + "."
                                                                 : quest.named_row,
                                         quest.named_reply, std::string(kQuestNamed)});
                }
                return plan;
            }
            if (quest.has_expose()) {
                // The holder with a secret (Words Must Not Fail's priest, notes.md 21 Oct
                // 2026): the giver knows he will not be asked, and that he hides where he
                // sleeps; the rest is the mod's Watch (QuestFacts::seen) and the game's menu.
                const std::string& she = quest.seen_with_name;
                plan.block += " You know " + holder + " will never give it up for the asking" +
                              (facts.rebuffed ? ", and they have been to him and found that out; you expected no better"
                                              : "") +
                              ". What you also know is that " + holder + " hides where he spends his nights, "
                              "and a man with a secret of his own can be made to give up somebody else's. You "
                              "do not know what the secret is";
                plan.block += facts.seen
                    ? ", but they have found it out: they saw him in the night in " + she + "'s bed, where "
                      "he should not be. You do not want it told to you, only the thing back; whatever "
                      "hold they have over him is theirs to use."
                    : ", only that there is one, and you tell them to find out where he sleeps.";
                return plan;
            }
            if (quest.dispute && facts.broker) {
                // The player has been to the holder and comes back carrying his condition.
                // What the giver says to it is written; whether he gives in is decided here,
                // once, so the row and the reply cannot disagree.
                if (facts.apologised) {
                    plan.block += " They have been to " + holder + " about it, and " + holder +
                                  " wanted something said first; you swallowed your pride and "
                                  "said it, and they carry your word to him. Do not go back "
                                  "on it, and do not dwell on it.";
                } else {
                    plan.block += " They have been to " + holder + " about it, and " + holder +
                                  " wants something from you before he will part with it";
                    plan.block += facts.refused
                        ? "; they put it to you once already and you would not give it. If they "
                          "put it to you again, you have your answer ready."
                        : ". If they put it to you, you have your answer ready.";
                    if (!quest.apologise.empty()) {
                        const bool relents = giver_relents(quest, approach, met);
                        plan.apologise_row = quest.apologise;
                        plan.apologise_reply = relents ? quest.apologise_yes : quest.apologise_no;
                        plan.apologise_command =
                            std::string(relents ? kQuestApologise : kQuestRefuseApology);
                    }
                }
            }
            return plan;
        }
        if (facts.locked) {
            // Not this one yet: the link before it is still out, and the giver asks one
            // thing at a time.
            plan.block = "You also want " + item + " back from " + holder +
                         ", but one thing at a time; you will not ask it of anyone until "
                         "the first errand is done, and you do not bring it up.";
            return plan;
        }
        plan.block = "Nobody has yet agreed to get " + item + " back from " + holder + "; " +
                     (money ? "you would give " + pay + " to whoever does."
                            : quest.reward_text.empty()
                                  ? "you have no money to give for it, only your thanks."
                                  : "you have no money to give for it, but whoever does would "
                                    "have " + pay + ".") +
                     before +
                     " If this person offers to, take them up on it and " +
                     (money ? "say there is money in it, without naming a sum."
                            : quest.reward_text.empty() ? "be plain that thanks is all you have."
                                                        : "name what you can give.");
        if (quest.dispute) {
            plan.block += " " + holder + " has a grievance against you over it, which you do "
                          "not accept; if it comes up, you give your own side and no more.";
        }
        if (!quest.offer.empty()) {
            plan.block += " When it comes up, this is how you put it: " + in_quotes(quest.offer);
        }
        plan.question = "Did this person just agree, offer or promise to get " + item +
                        " back from " + holder + " for you?";
        plan.on_yes = std::string(kQuestAccept);
        plan.direction = "[They have agreed to fetch it. You are relieved, and you make sure "
                         "they know it is " + pay + " when it is back in your hands.]";
        return plan;
    }

    // The holder. Whether they give the thing up is not decided here at all: while the
    // quest is open the game offers its own menu on them and weighs Henry's persuasion,
    // threat or money itself, so this conversation only ever happens either side of that
    // -- before anyone has asked them on the owner's behalf, or after the thing has gone.
    const std::string owner = quest.giver_name;
    const std::string thing = owner + "'s " + quest.item_name;
    if (facts.handed || facts.done || carrying) {
        plan.block = "You gave " + thing + " up to this person, who came for it on " + owner +
                     "'s behalf. It is out of your hands now and you would sooner not talk "
                     "about it.";
        return plan;
    }
    // A letter brought to the holder and kept (item_of) was never borrowed: he has it
    // because he would not let it go, and he means it for a use of his own.
    plan.block = "You have " + thing +
                 (quest.item_of.empty()
                      ? ", borrowed and never given back, and you keep finding reasons not to."
                      : ", which was brought to you and which you kept, and you mean to keep it; "
                        "you have a use for it.") +
                 " If this person brings it up, make your excuses and keep it; "
                 "nothing said in passing is going to part you from it.";
    if (quest.unyielding) {
        plan.block += std::string(" Being asked for it at all offends you") +
                      (facts.rebuffed ? ", and this person has asked already and been told; "
                                        "if they try again you are shorter with them still."
                                      : "; you take it as an accusation, and say so, coldly.") +
                      " No argument, threat or offer of money moves you, and you would sooner "
                      "end the conversation than hear one out.";
    }
    if (quest.has_expose()) {
        // The holder's own secret (Words Must Not Fail's priest): nobody knows where he
        // sleeps, and he means to keep it so. Caught (the mod's Watch, flag s), he is
        // frightened; the giving-in itself is the game's menu on him, not this talk.
        const std::string& she = quest.seen_with_name;
        plan.block += facts.seen
            ? " This person found you in the night where you should never have been, in " + she +
                  "'s bed, and you know it. You are frightened of what they could do with that, and it "
                  "shows: you bluster, you lower your voice, you ask what they want. You do not deny "
                  "it, and you do not give the thing up here; that is said between you where the game "
                  "has its own words for it."
            : " You have a secret of your own: your nights are not spent where a man of your standing's "
                  "should be, and nobody knows it. You never speak of where you sleep, and a question "
                  "about it makes you short.";
    }
    if (quest.dispute) {
        plan.block += " Between you and " + owner + " there is a grievance over it, and you "
                      "hold that " + owner + " is in the wrong.";
    }
    return plan;
}

}  // namespace

std::size_t pick_quest(const std::vector<Quest>& quests, const std::vector<QuestFacts>& facts) {
    // In order of how much the conversation turns on it. A giver's favour first, since that
    // is the one the app has words for; the holder's side is the game's menu while it is open.
    const auto find = [&](auto&& want) {
        for (std::size_t i = 0; i < quests.size() && i < facts.size(); ++i) {
            if (want(quests[i], facts[i])) return i;
        }
        return quests.size();
    };
    const auto giver = [](const Quest& q) { return q.role == QuestRole::Giver; };
    std::size_t at = find([&](const Quest& q, const QuestFacts& f) {
        return giver(q) && !f.done && (q.is_find() ? f.accepted : true) && q.brought(f.have);
    });
    if (at < quests.size()) return at;
    at = find([&](const Quest& q, const QuestFacts& f) { return giver(q) && f.accepted && !f.done; });
    if (at < quests.size()) return at;
    at = find([&](const Quest& q, const QuestFacts& f) { return giver(q) && offerable(q, f); });
    if (at < quests.size()) return at;
    // The one a travel looks at is party to nothing, and is never the quest of the moment.
    at = find([&](const Quest& q, const QuestFacts& f) {
        return q.role == QuestRole::Target && !q.is_travel() && f.accepted && !f.handed && !f.done;
    });
    if (at < quests.size()) return at;
    // A later link's holder is not yet anybody's holder: a locked or void scene plans
    // nothing, and the settled one before it has the paragraph.
    at = find([&](const Quest& q, const QuestFacts& f) {
        return q.role == QuestRole::Target && !q.is_travel() && !f.handed && !f.done && !f.locked && !f.voided;
    });
    if (at < quests.size()) return at;
    // The one an ask's word is carried on to, while the word is on its way to them.
    at = find([&](const Quest& q, const QuestFacts& f) {
        return q.role == QuestRole::Onward && f.onward && !f.done;
    });
    if (at < quests.size()) return at;
    // Everything settled: the first, whose paragraph says so -- passing over a travel's
    // looked-at side, which has no paragraph worth the name. Also 0 == size() when empty.
    at = find([&](const Quest& q, const QuestFacts&) {
        return !(q.role == QuestRole::Target && q.is_travel());
    });
    return at < quests.size() ? at : 0;
}

std::string quest_aside(const Quest& quest, const QuestFacts& facts,
                        const std::vector<Quest>& all, const std::vector<QuestFacts>& all_facts) {
    if (!quest.exists() || facts.voided) return {};
    if (quest.is_travel()) {
        if (quest.role != QuestRole::Giver) return {};
        if (facts.done) {
            return "This person also went and looked at " + quest.target_name +
                   " for you, which is settled.";
        }
        if (facts.accepted) {
            return "They have also agreed to go and see how things stand with " + quest.target_name +
                   ", and have not yet brought you word.";
        }
        return {};
    }
    if (quest.is_beat()) {
        if (quest.role != QuestRole::Giver) {
            if (facts.beaten) return "This person also beat you, on " + quest.giver_name + "'s account.";
            return {};
        }
        if (facts.done) return "This person also dealt with " + quest.target_name + " for you, which is settled.";
        if (facts.accepted) {
            return "They have also agreed to go and make " + quest.target_name +
                   " make things right with you, and have not yet told you how it went.";
        }
        return {};
    }
    if (quest.is_spot()) {
        if (quest.role != QuestRole::Giver) return {};
        const std::string where = quest.spot_text.empty() ? std::string("the place you named") : quest.spot_text;
        if (facts.done) {
            return "This person also took your " + quest.item_name + " to " + where + " for you, which is settled.";
        }
        if (facts.accepted) {
            return "They also have your " + quest.item_name + " and have agreed to take it to " + where +
                   ", and have not yet told you they have.";
        }
        return {};
    }
    if (quest.is_pray()) {
        if (quest.role != QuestRole::Giver) return {};
        if (facts.done) return "This person also said a prayer for you, which is settled.";
        if (facts.accepted) {
            return "They have also agreed to go and pray for you at " +
                   (quest.spot_text.empty() ? std::string("the place you named") : quest.spot_text) +
                   ", and have not yet told you they have.";
        }
        return {};
    }
    if (quest.is_ask()) {
        if (quest.role == QuestRole::Onward) {
            if (facts.done) {
                return "This person also brought you word from " + quest.giver_name + " of what " +
                       quest.target_name + " had been keeping back.";
            }
            return {};
        }
        if (quest.role != QuestRole::Giver) {
            if (facts.handed || facts.done) {
                return "This person also got the truth of a matter out of you, on " + quest.giver_name + "'s account.";
            }
            return {};
        }
        if (facts.done) return "This person also went to " + quest.target_name + " for you, which is settled.";
        if (quest.carries_on() && facts.onward) {
            return "They also got the truth out of " + quest.target_name + " for you and are carrying it "
                   "on to " + quest.onward_name + " as you asked; you have paid them already.";
        }
        if (facts.accepted) {
            return "They have also agreed to get the truth of a matter out of " + quest.target_name +
                   " for you, and have not yet brought you word.";
        }
        return {};
    }
    if (quest.is_ambush()) {
        if (quest.role != QuestRole::Giver) {
            if (facts.handed || facts.done) {
                return "This person also got the way to the one who lifts curses out of you, on " +
                       quest.giver_name + "'s account, after you had sent them wrong once.";
            }
            if (facts.directed) {
                return "You also wrote this person a way down, on " + quest.giver_name +
                       "'s account, and it was not the true one.";
            }
            return {};
        }
        if (facts.done) return "This person also went to " + quest.target_name + " for you, which is settled.";
        if (facts.accepted) {
            return "They have also agreed to get the way to one who lifts curses out of " + quest.target_name +
                   " for you, and have not yet brought you word.";
        }
        return {};
    }
    if (quest.is_buy()) {
        if (quest.role != QuestRole::Giver) {
            if (facts.handed || facts.done) {
                return "This person also bought a " + quest.item_name + " off you, on " + quest.giver_name + "'s account.";
            }
            return {};
        }
        if (facts.done) {
            return "This person also brought you the " + quest.item_name + " from " + quest.target_name +
                   ", which is settled.";
        }
        if (facts.handed || facts.have > 0) {
            return "They have the " + quest.item_name + " from " + quest.target_name + " on them too.";
        }
        if (facts.accepted) {
            return "They also have your hood and your money, and have agreed to bring back the " +
                   quest.item_name + " from " + quest.target_name + ".";
        }
        return {};
    }
    if (quest.is_mind()) {
        if (quest.role != QuestRole::Giver) {
            if (facts.checks > 0) return "This person also looks in on you, on " + quest.giver_name + "'s account.";
            return {};
        }
        if (facts.done) return "This person also looked in on " + quest.target_name + " for you, which is settled.";
        if (facts.accepted) {
            return "They have also agreed to look in on " + quest.target_name +
                   " for you, and have not yet brought you word.";
        }
        return {};
    }
    if (quest.is_find()) {
        if (quest.role != QuestRole::Giver) return {};
        const std::string things = std::to_string(quest.count) + " " + quest.item_name;
        if (facts.done) return "This person also brought you " + things + ", which is settled.";
        if (facts.accepted) {
            return "They have also agreed to bring you " + things + ", and have not brought them yet.";
        }
        return {};
    }
    if (quest.is_deliver()) {
        const std::string what = quest.carries_word() ? "word" : quest.carries_thing() ? "the " + quest.item_name : "letter";
        if (quest.role == QuestRole::Giver) {
            if (facts.done) {
                return quest.carries_thing()
                    ? "This person also took " + what + " back to " + quest.target_name + " for you, which is settled."
                    : "This person also carried your " + what + " to " + quest.target_name + " for you, which is settled.";
            }
            if (facts.accepted) {
                return quest.carries_thing()
                    ? "They have also agreed to take " + what + " back to " + quest.target_name +
                      ", and have not yet told you it is there."
                    : "They have also agreed to carry your " + what + " to " + quest.target_name +
                      ", and have not yet told you it is there.";
            }
            return {};
        }
        if (facts.handed || facts.done) {
            return quest.carries_thing()
                ? "This person also brought your " + quest.item_name + " back to you."
                : std::string("This person also brought you ") +
                  (quest.carries_word() ? "word" : "a letter") + " from " + quest.giver_name + ".";
        }
        return {};
    }
    if (quest.role == QuestRole::Giver) {
        const std::string item = "your " + quest.item_name;
        if (facts.done) {
            if (facts.how == "lied") {
                // The lie_have row believed: nothing came back, and the giver has given it up.
                return "This person also went to " + quest.target_name + " for " + item +
                       " and came back saying he never had it; you believed them, and the thing is lost to you.";
            }
            return "This person also got " + item + " back from " + quest.target_name +
                   " for you, which is settled.";
        }
        if (facts.have > 0) {
            return "They have " + item + " on them too, which " + quest.target_name + " had.";
        }
        if (facts.accepted) {
            return "They have also agreed to get " + item + " back from " + quest.target_name +
                   ", and have not brought it yet.";
        }
        // Not yet asked: a later link waits its turn and is not spoken of; a favour that
        // could be asked is for its own conversation, and the model is not told two offers.
        return {};
    }
    const std::string thing = quest.giver_name + "'s " + quest.item_name;
    // A kept letter (item_of) is in the holder's hands only once the scene that wrote it has
    // ended his way; until then there is nothing for him to have.
    if (!quest.item_of.empty() && facts.locked) return {};
    if (facts.handed || facts.done || facts.have > 0) {
        return "You gave " + thing + " up to this person, who came for it on " + quest.giver_name +
               "'s behalf.";
    }
    std::string out = "You also have " + thing + (quest.item_of.empty() ? ", borrowed and never given back"
                                                                        : ", brought to you and kept");
    if (const Quest* inner = inner_of(quest, all)) {
        // The nest: the thing goes home when the holder's own favour is done, and not before.
        bool inner_done = false;
        for (std::size_t i = 0; i < all.size() && i < all_facts.size(); ++i) {
            if (all[i].id == inner->id) inner_done = all_facts[i].done;
        }
        out += inner_done
            ? ", and now that your own " + inner->item_name + " is home you have no more "
              "excuse to keep it; if they ask, you will give it up."
            : ", and you will not part with it until your own " + inner->item_name +
              " is back from " + inner->target_name + ".";
        return out;
    }
    return out + ", and you keep finding reasons not to.";
}

std::string quest_directive(const Quest& quest, std::string_view command) {
    return "@quest " + quest.id + " " + std::string(command);
}

std::vector<std::string> beats_of(std::string_view words) {
    // A blank line -- a newline, any spaces, another newline -- is the break between beats;
    // single newlines are the writer's wrapping and stay with the words. Empty beats drop,
    // so stray blank lines at either end do not make a silent turn.
    std::vector<std::string> out;
    std::string beat;
    std::size_t i = 0;
    while (i < words.size()) {
        if (words[i] == '\n') {
            std::size_t j = i + 1;
            while (j < words.size() && (words[j] == ' ' || words[j] == '\t' || words[j] == '\r')) ++j;
            if (j < words.size() && words[j] == '\n') {
                out.push_back(beat);
                beat.clear();
                i = j + 1;
                continue;
            }
        }
        beat += words[i++];
    }
    out.push_back(beat);
    std::vector<std::string> kept;
    for (std::string& b : out) {
        const std::size_t from = b.find_first_not_of(" \t\r\n");
        if (from == std::string::npos) continue;
        const std::size_t to = b.find_last_not_of(" \t\r\n");
        kept.push_back(b.substr(from, to - from + 1));
    }
    if (kept.empty()) kept.emplace_back(words);
    return kept;
}

Verdict read_verdict(std::string_view reply) {
    // The first word, lowered and stripped of anything that is not a letter, since a model
    // asked for one word will still put a full stop after it, or quote it.
    std::string word;
    bool started = false;
    for (const char c : reply) {
        const bool letter = std::isalpha(static_cast<unsigned char>(c)) != 0;
        if (letter) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            started = true;
        } else if (started) {
            break;
        }
    }
    if (word == "yes" || word == "aye" || word == "yeah" || word == "ay") return Verdict::Yes;
    if (word == "no" || word == "nay" || word == "nope") return Verdict::No;
    // A model that answered in a sentence anyway. The whole reply is looked at for either
    // word on its own, and a reply that has both is nobody's verdict.
    std::string low;
    for (const char c : reply) {
        low += std::isalpha(static_cast<unsigned char>(c))
                   ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
                   : ' ';
    }
    std::istringstream words(low);
    bool yes = false, no = false;
    for (std::string one; words >> one;) {
        if (one == "yes" || one == "aye") yes = true;
        if (one == "no" || one == "nay") no = true;
    }
    if (yes && !no) return Verdict::Yes;
    if (no && !yes) return Verdict::No;
    return Verdict::Unclear;
}

std::vector<Message> judge_messages(std::vector<Message> conversation,
                                    std::string_view question) {
    // The player's words as they stand, then the question beneath them. Out of character
    // and in brackets, the same shape as the directions the prompt already uses, so it is
    // read as the stage's voice rather than the player's.
    const std::string asked = "\n\n(Answer this one question with a single word, YES or NO, and "
                              "nothing else: " + std::string(question) + ")";
    if (conversation.empty() || conversation.back().role != "user") {
        conversation.push_back({"user", asked.substr(2)});
    } else {
        conversation.back().content += asked;
    }
    return conversation;
}

Options judge_options(const Options& options) {
    Options cold = options;
    cold.reply_tokens = 4;
    cold.temperature = 0.0;
    cold.top_k = 1;
    return cold;
}

}  // namespace moonshine
