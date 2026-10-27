#include "scripted.h"

#include <algorithm>
#include <initializer_list>
#include <cctype>

namespace moonshine {

namespace {

constexpr std::string_view kAskPrefix = "ask:";
constexpr std::string_view kSayPrefix = "say:";

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string lower_first(std::string label) {
    if (!label.empty()) label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
    return label;
}

// The generator writes a label as a name ("Drahomira", "Mikesh the foreman", "Young Ludwig"),
// as a capitalised phrase ("Begging at the market", "The other beggars") or as a phrase in the
// third person ("his trade", "her husband Paul"). Inside a sentence the phrases lose their
// capital and the names keep theirs; the first word is what tells them apart, and a name is
// never an article, a possessive or a gerund.
std::string in_sentence(std::string_view label, std::string_view whose) {
    std::string out(label);
    const std::size_t space = out.find(' ');
    const std::string first = out.substr(0, space);
    std::string low = first;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static constexpr std::string_view kCommon[] = {"the", "a",   "an",   "his",  "her",
                                                   "my",  "our", "their", "your", "that",
                                                   "this", "what", "how"};
    const bool common = std::find(std::begin(kCommon), std::end(kCommon), low) != std::end(kCommon);
    const bool gerund = low.size() > 4 && low.compare(low.size() - 3, 3, "ing") == 0;
    if (common || gerund) out = lower_first(out);
    // "his trade" is the writer's; in a mouth it is "my trade" or "your trade".
    if (low == "his" || low == "her") {
        out = std::string(whose) + (space == std::string::npos ? "" : out.substr(space));
    }
    return out;
}

// A topic label Henry could not have asked for cold: a neighbour's name ("Baldwin's wet
// ledger", "Drahomira", "Mikesh the foreman"). The soul may still offer those as rows;
// they must not appear in the spoken list as if Henry already knew the names
// (notes.md, 5 Oct 2026).
bool label_names_someone(std::string_view label) {
    if (label.empty()) return false;
    // "Baldwin's wet ledger", not "the clerk's trade".
    for (std::size_t i = 1; i + 1 < label.size(); ++i) {
        if (label[i] == '\'' && label[i + 1] == 's' &&
            std::isupper(static_cast<unsigned char>(label[0]))) {
            return true;
        }
    }
    const std::size_t space = label.find(' ');
    const std::string first(label.substr(0, space));
    if (first.empty() || !std::isupper(static_cast<unsigned char>(first[0]))) return false;
    std::string low = first;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static constexpr std::string_view kCommon[] = {"the", "a",   "an",   "his",  "her",
                                                   "my",  "our", "their", "your", "that",
                                                   "this", "what", "how"};
    if (std::find(std::begin(kCommon), std::end(kCommon), low) != std::end(kCommon)) return false;
    const bool gerund = low.size() > 4 && low.compare(low.size() - 3, 3, "ing") == 0;
    return !gerund;
}

// The words of a label or a name, letters only: "Alice's" is Alice, "Humphrey," is Humphrey.
std::vector<std::string> words_of(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    for (const char c : text) {
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '-') {
            word += c;
        } else if (!word.empty()) {
            out.push_back(word);
            word.clear();
        }
    }
    if (!word.empty()) out.push_back(word);
    // The possessive's s comes off as a word of its own; it is nobody's name.
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& w) { return w == "s"; }), out.end());
    return out;
}

}  // namespace

bool Standing::offerable(std::string_view quest_id) const {
    if (quest_id.empty()) return false;
    return quests.find(std::string(quest_id)) == quests.end();
}

bool Standing::is_done(std::string_view quest_id) const {
    const auto at = quests.find(std::string(quest_id));
    return at != quests.end() && at->second == "done";
}

QuestFacts Standing::facts_of(std::string_view quest_id, int fallback) const {
    QuestFacts facts;
    const std::string id(quest_id);
    if (const auto at = quests.find(id); at != quests.end()) {
        // The furthest stage only; done was accepted, and may or may not have been handed
        // (a thing got without the holder's say-so is done unhanded).
        facts.done = at->second == "done";
        facts.handed = at->second == "handed";
        facts.accepted = facts.done || facts.handed || at->second == "accepted";
    }
    if (const auto at = flags.find(id); at != flags.end()) {
        facts.broker = at->second.find('b') != std::string::npos;
        facts.apologised = at->second.find('p') != std::string::npos;
        facts.refused = at->second.find('r') != std::string::npos;
        facts.seen = at->second.find('s') != std::string::npos;
        facts.beaten = at->second.find('k') != std::string::npos;
        facts.dead = at->second.find('x') != std::string::npos;
        facts.prayed = at->second.find('y') != std::string::npos;
        facts.left = at->second.find('l') != std::string::npos;
        facts.heard = at->second.find('h') != std::string::npos;
        facts.looted = at->second.find('o') != std::string::npos;
        facts.cursed = at->second.find('u') != std::string::npos;
        facts.directed = at->second.find('g') != std::string::npos;
        facts.ambushed = at->second.find('m') != std::string::npos;
        facts.onward = at->second.find('w') != std::string::npos;
        facts.keepsake = at->second.find('e') != std::string::npos;
        facts.rebuffed = at->second.find('n') != std::string::npos;
        facts.named = at->second.find('a') != std::string::npos;
        facts.letin = at->second.find('i') != std::string::npos;
        facts.keyed = at->second.find('q') != std::string::npos;
        facts.denounced = at->second.find('d') != std::string::npos;
        facts.checks = static_cast<int>(std::count(at->second.begin(), at->second.end(), 'c'));
        // W and the digits after it: a dictated letter's outcomes (the mod's QuestFlagLetters
        // puts them last, so the digits run to the end).
        if (const std::size_t w = at->second.find('W'); w != std::string::npos) {
            for (std::size_t i = w + 1; i < at->second.size(); ++i) {
                const char c = at->second[i];
                if (c < '0' || c > '9') break;
                facts.wrote += c;
            }
        }
    }
    if (const auto at = hows.find(id); at != hows.end()) facts.how = at->second;
    if (const auto at = ages.find(id); at != ages.end()) facts.hours_since_accept = at->second;
    const auto count = holding.find(id);
    facts.have = count != holding.end() ? count->second : fallback;
    return facts;
}

Trust trust_tier(const Standing& standing, const QuestFacts& own) {
    if ((own.done && standing.met >= 3) || standing.trust >= kConfidedTrust) return Trust::Confided;
    if (standing.met >= 2 || own.done) return Trust::Friendly;
    return Trust::Open;
}

bool trust_allows(Trust tier, std::string_view tag) {
    if (tag == "open") return true;
    if (tag == "friendly") return tier != Trust::Open;
    if (tag == "confided") return tier == Trust::Confided;
    return false;
}

std::string_view trust_word(Trust tier) {
    switch (tier) {
        case Trust::Open: return "open";
        case Trust::Friendly: return "friendly";
        case Trust::Confided: return "confided";
    }
    return "open";
}

std::string_view kb_gate_word(KbGate gate) {
    switch (gate) {
        case KbGate::Open: return "open";
        case KbGate::Asking: return "asking";
        case KbGate::Owed: return "owed";
        case KbGate::Shut: return "shut";
    }
    return "open";
}

Scripted plan_scripted(std::string_view self_account, const std::vector<Knowledge>& knowledge,
                       const std::vector<HelpReply>& help_replies, const Quest& quest,
                       const QuestFacts& facts, const Standing& standing,
                       const std::vector<Quest>& others,
                       const std::vector<QuestFacts>& others_facts,
                       const std::vector<Remark>& remarks, std::string_view about_again,
                       const std::vector<Person>& people) {
    Scripted out;
    // Every favour of theirs with its facts, the conversation's first.
    std::vector<std::pair<const Quest*, const QuestFacts*>> every;
    if (quest.exists()) every.emplace_back(&quest, &facts);
    for (std::size_t i = 0; i < others.size() && i < others_facts.size(); ++i) {
        if (others[i].exists() && others[i].id != quest.id) {
            every.emplace_back(&others[i], &others_facts[i]);
        }
    }
    const bool giver = quest.exists() && quest.role == QuestRole::Giver;
    // The same reading plan_quest makes: nobody has agreed yet, nothing is in the player's
    // hands and no earlier link is out, so the favour is there to be asked.
    const bool offerable = giver && moonshine::offerable(quest, facts);
    // The accepting row is the app's own, composed from the favour's fields, so a giver
    // whose offer is the model's (no text written for the favour) still has a row that
    // takes it up on a pick; the model writes no rows.
    if (offerable) {
        out.offer = quest.offer;
        // The row that draws the offer out, in the quest's own words where it has them.
        out.help_row = quest.opener;
        out.accept_row = quest.carries_word()
            ? "I'll go to " + quest.target_name + " and tell him for you."
            : quest.has_dictation()
            ? "I'll write your " + quest.item_name + " down for you and take it to " + quest.target_name + "."
            : quest.is_deliver()
            ? "I'll take your " + quest.item_name + " to " + quest.target_name + "."
            : quest.is_find()
            ? "I'll get you the " + quest.item_name + " you want."
            : quest.is_travel()
            ? "I'll go and look in on " + quest.target_name + " for you."
            : quest.is_beat()
            ? "I'll go and have a word with " + quest.target_name + " for you, and see he makes it right."
            : quest.is_pray()
            ? "I'll go and say a prayer for you" +
                  (quest.spot_text.empty() ? std::string() : " at " + quest.spot_text) + "."
            : quest.is_spot()
            ? "I'll take your " + quest.item_name + (quest.is_return() ? " back to " : " to ") +
                  (quest.spot_text.empty() ? std::string("the place") : quest.spot_text) + " for you."
            : quest.is_mind()
            ? "I'll look in on " + quest.target_name + " for you, and keep looking in."
            : quest.is_ask()
            ? "I'll go to " + quest.target_name + " and get the truth of it for you."
            : "I'll get your " + quest.item_name + " back from " + quest.target_name + ".";
        // Taken up, the giver answers with the way there (`agreed`), or for a quest written
        // before that line with the second telling: the errand is taken and answered without
        // the model, in voice, at once. And Henry's one question under the offer, answered
        // the same way, while both were written.
        out.accept_reply = !quest.agreed.empty() ? quest.agreed : quest.offer_2;
        if (!quest.more.empty() && !quest.more_reply.empty()) {
            out.more_row = quest.more;
            out.more_reply = quest.more_reply;
        }
        if (!quest.more_2.empty() && !quest.more_2_reply.empty()) {
            out.more_2_row = quest.more_2;
            out.more_2_reply = quest.more_2_reply;
        }
    }
    // A letter's writer may be asked what is in it from the offer on (notes.md 9 Oct 2026:
    // not gated on holding the letter) until it is handed over: both answers written, or no
    // row. The factor is the game's own hard one (1.5) for a hard quest, and gentler below.
    if (giver && quest.is_deliver() && !quest.carries_word() && !facts.handed && !facts.done &&
        !facts.locked && !facts.voided && (offerable || facts.accepted) && !quest.pry.empty() &&
        !quest.pry_yes.empty() && !quest.pry_no.empty()) {
        out.pry_row = quest.pry;
        out.pry_yes = quest.pry_yes;
        out.pry_no = quest.pry_no;
        out.pry_factor = quest.difficulty == "hard" ? 1.5 : quest.difficulty == "easy" ? 1.0 : 1.25;
    }
    // Taken up and not yet brought home, the opening round's offer of help is the wrong
    // question from the man who said he would; he may ask to be reminded instead, and is
    // told the offer again, which names the thing, who has it and what it pays.
    if (giver && facts.accepted && !facts.done && !facts.locked) {
        out.remind = !quest.offer.empty() ? quest.offer : quest.offer_2;
    }

    // The written `help` replies are for a soul with nothing to ask: a refusal, a lead, or a
    // giver's "I have my tools back". While a giver's own errand is in play -- offerable but
    // with no written offer, agreed and not yet brought, or the thing just now in Henry's
    // hands -- the model has the quest paragraph for it and these would contradict it.
    const bool errand_in_play = giver && !facts.done && !facts.locked;
    if (!errand_in_play && !help_replies.empty()) {
        // A different variant each meeting, and a lead only while what it leads to is still
        // there; a lead gone stale falls through to the next reply that is not.
        const std::size_t n = help_replies.size();
        const std::size_t start = static_cast<std::size_t>(std::max(standing.met - 1, 0)) % n;
        for (std::size_t i = 0; i < n; ++i) {
            const HelpReply& reply = help_replies[(start + i) % n];
            if (reply.text.empty()) continue;
            if (!reply.refers_to_quest_id.empty() && !standing.offerable(reply.refers_to_quest_id)) {
                continue;
            }
            out.help = reply.text;
            // A lead to a favour still to be had is a reason to ask; a refusal is not.
            out.help_due = !reply.refers_to_quest_id.empty();
            break;
        }
    }
    // And the soul's own errand, there to be taken, whether or not its offer is written.
    if (offerable) out.help_due = true;

    // The account of themselves is for a stranger; somebody they have given it gets the line
    // written for being asked again, which the save's memory of the first telling bears out.
    if (standing.first_meeting()) {
        out.about = std::string(self_account);
    } else {
        out.about = std::string(about_again);
        out.about_told = !self_account.empty();
    }

    // What they know was written with the favour where the deal left it: the holder has the
    // axe, the giver wants it back. Once the thing has moved, a topic about the other party
    // or the thing says what is no longer so ("Aye, I've got his work axe"), and is dropped.
    // The generator gives a topic no quest to hang on, so the other party's name in the key
    // or the label, or the thing's, is what marks it; a topic about a third soul stands.
    // Every favour of theirs counts, not only the conversation's.
    const auto stale_words = [&](std::initializer_list<std::string_view> texts) {
        for (const auto& [q, f] : every) {
            if (!(f->handed || f->done)) continue;
            const std::string other = lower(q->role == QuestRole::Giver ? q->target_name : q->giver_name);
            const std::string thing = lower(q->item_name);
            for (std::string_view text : texts) {
                const std::string low = lower(text);
                if (!other.empty() && low.find(other) != std::string::npos) return true;
                if (!thing.empty() && low.find(thing) != std::string::npos) return true;
            }
        }
        return false;
    };
    const auto stale = [&](const Knowledge& item) { return stale_words({item.key, item.label}); };

    // A favour of theirs done, whichever it was, is what earns the trust.
    QuestFacts own = facts;
    for (const auto& [q, f] : every) {
        if (q->role == QuestRole::Giver && f->done) own.done = true;
    }
    const Trust tier = trust_tier(standing, own);
    out.tier = tier;
    // And a favour of theirs done, whichever it was, is what opens what they know to a typed
    // or spoken question: their knowledge is the help the favour pays for. Until then the
    // gate reads off the conversation's favour where it is theirs to give, else the first
    // of their others; a favour they never have to ask (voided, or a prayer the player
    // cannot say) is no favour and shuts nothing.
    out.kb_gate = KbGate::Open;
    if (!own.done) {
        for (const auto& [q, f] : every) {
            if (q->role != QuestRole::Giver || f->voided || f->cannot_pray) continue;
            out.kb_gate = f->accepted                 ? KbGate::Owed
                          : moonshine::offerable(*q, *f) ? KbGate::Asking
                                                         : KbGate::Shut;
            break;
        }
    }
    out.people = people;
    // A topic that waits on a favour's ending (Knowledge::after_quest): the quest's facts
    // from this soul's own list where it is theirs, else from the save, and the row is
    // theirs to offer only once that favour is done the way named.
    const auto knows_yet = [&](const Knowledge& item) {
        if (item.after_quest.empty()) return true;
        const QuestFacts* known = nullptr;
        for (const auto& [q, f] : every) {
            if (q->id == item.after_quest) known = f;
        }
        const QuestFacts fallback = known ? *known : standing.facts_of(item.after_quest);
        return fallback.done && how_matches(fallback.how, item.after_how);
    };
    for (const Knowledge& item : knowledge) {
        if (item.text.empty() || stale(item) || !knows_yet(item)) continue;
        if (!trust_allows(tier, item.trust)) {
            out.withheld = true;
            continue;
        }
        Topic topic{item.key, item.label, item.text, topic_person(item.label, people)};
        // A topic about somebody the circle does not list is still about a name, and the
        // name has to be heard for the row to open, so the lines are read for it too.
        if (topic.about.rfind("name:", 0) == 0) {
            bool listed = false;
            for (const Person& p : out.people) listed = listed || p.token == topic.about;
            if (!listed) {
                std::string first(item.label.substr(0, item.label.find(' ')));
                if (const std::size_t at = first.find('\''); at != std::string::npos) first.resize(at);
                out.people.push_back({topic.about, first});
            }
        }
        out.topics.push_back(std::move(topic));
    }
    // A remark's answer was written with the favour where the deal left it too, and one
    // that speaks of the other party or the thing is dropped the same way once it has
    // moved; the words themselves are what mark it, there being no key or label to read.
    for (const Remark& remark : remarks) {
        if (remark.row.empty() || remark.text.empty()) continue;
        if (stale_words({remark.row, remark.text})) continue;
        out.remarks.push_back(remark);
    }
    return out;
}

std::string given_name(std::string_view name) {
    static constexpr std::string_view kTitles[] = {"Father", "Brother", "Sister", "Sir",  "Master",
                                                   "Mistress", "Old",    "Young",  "Dame", "Lord", "Lady"};
    const std::vector<std::string> words = words_of(name);
    for (const std::string& w : words) {
        if (std::find(std::begin(kTitles), std::end(kTitles), w) == std::end(kTitles)) return w;
    }
    return words.empty() ? std::string() : words.front();
}

std::string name_token(std::string_view name) {
    std::string out = "name:";
    for (const char c : given_name(name)) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string topic_person(std::string_view label, const std::vector<Person>& people) {
    for (const std::string& word : words_of(label)) {
        if (!std::isupper(static_cast<unsigned char>(word[0]))) continue;
        for (const Person& person : people) {
            if (!person.name.empty() && word == person.name) return person.token;
        }
    }
    if (!label_names_someone(label)) return {};
    return name_token(label);
}

std::vector<std::string> people_named(std::string_view words, const std::vector<Person>& people) {
    std::vector<std::string> out;
    const auto boundary = [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); };
    for (const Person& person : people) {
        if (person.name.empty()) continue;
        if (std::find(out.begin(), out.end(), person.token) != out.end()) continue;
        std::size_t at = words.find(person.name);
        while (at != std::string_view::npos) {
            const bool before = at == 0 || boundary(words[at - 1]);
            const std::size_t end = at + person.name.size();
            const bool after = end >= words.size() || boundary(words[end]);
            if (before && after) {
                out.push_back(person.token);
                break;
            }
            at = words.find(person.name, at + 1);
        }
    }
    return out;
}

// The soul offers what they know rather than quizzing Henry on it: a topic's label may
// name a neighbour Henry has never heard of ("Baldwin's wet ledger"), and "Ask, then. The
// clerk's trade, Baldwin's wet ledger, or Wilhelm's humour?" read as if he should know
// them (noted 5 Oct 2026). Said as an offer, the name is the soul's to drop and Henry's
// row ("Tell me about Baldwin's wet ledger.") picks up what was just said.
std::string ask_reply(const std::vector<Topic>& topics, unsigned opener) {
    // The reply used to recite the rows -- "Ask then. I can tell you about working the
    // holding, my late husband, or sitting on the bench" -- which no one says of themselves
    // and which the rows under it show anyway (notes.md, 8 Oct 2026). Now it is only the
    // leave to ask, varied so two neighbours do not say it alike; what there is to ask about
    // is Henry's to read off the list.
    static constexpr std::string_view kOpeners[] = {
        "Ask, then.",
        "What is it you want to know?",
        "Ask, then, though I know little but my own story.",
        "Depends what about. Ask.",
        "Ask away. I'll tell you what I can.",
    };
    (void)topics;
    return std::string(kOpeners[opener % (sizeof(kOpeners) / sizeof(kOpeners[0]))]);
}

std::string ask_row(const Topic& topic) {
    return "Tell me about " + in_sentence(topic.label, "your") + ".";
}

std::string ask_intent(std::string_view key) {
    return std::string(kAskPrefix) + std::string(key);
}

std::string_view ask_key(std::string_view intent) {
    if (intent.size() <= kAskPrefix.size() || intent.substr(0, kAskPrefix.size()) != kAskPrefix) {
        return {};
    }
    return intent.substr(kAskPrefix.size());
}

std::string say_intent(std::string_view key) {
    return std::string(kSayPrefix) + std::string(key);
}

std::string_view say_key(std::string_view intent) {
    if (intent.size() <= kSayPrefix.size() || intent.substr(0, kSayPrefix.size()) != kSayPrefix) {
        return {};
    }
    return intent.substr(kSayPrefix.size());
}

std::string alms_intent(std::string_view kind) {
    return "alms:" + std::string(kind);
}

std::string_view alms_kind(std::string_view intent) {
    constexpr std::string_view prefix = "alms:";
    if (intent.size() <= prefix.size() || intent.substr(0, prefix.size()) != prefix) return {};
    const std::string_view kind = intent.substr(prefix.size());
    if (kind != "money" && kind != "food" && kind != "clothes") return {};
    return kind;
}

}  // namespace moonshine
