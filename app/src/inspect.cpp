#include "inspect.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>

#include "prompt.h"
#include "quest.h"
#include "scripted.h"

namespace moonshine {

namespace {

// A boolean as JSON spells it.
std::string_view yes_no(bool value) {
    return value ? "true" : "false";
}

// The shortest spelling of a double that still reads back as itself. Sampling values are
// the only non-integers here, and they are read by people: top_p arriving as
// 0.95000000000000001 or as 0.94999999999999996 would both be taken as somebody's
// deliberate choice rather than as an artefact of printing 0.95.
std::string number(double value) {
    char text[32];
    for (int digits = 6; digits < 17; ++digits) {
        std::snprintf(text, sizeof text, "%.*g", digits, value);
        if (std::strtod(text, nullptr) == value) break;
    }
    return text;
}

}  // namespace

std::string json_string(std::string_view text) {
    std::string out = "\"";
    out.reserve(text.size() + 16);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Control characters are the only other thing JSON forbids raw. Everything
                // above them is passed through as the bytes it arrived as, which keeps the
                // UTF-8 in a Bohemian name intact rather than mangling it into escapes.
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escape[7];
                    std::snprintf(escape, sizeof escape, "\\u%04x", c);
                    out += escape;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string persona_json(const Soul& soul,
                         std::string_view world,
                         const Approach& approach,
                         const Doing& doing,
                         const Options& options,
                         std::size_t least_chars,
                         bool say_written,
                         const QuestFacts& facts,
                         const std::string& memory,
                         const Standing& standing) {
    const Persona persona =
        persona_for(soul, approach, doing, say_written, facts, memory, standing);

    std::string out = "{\n";
    auto line = [&out](std::string_view key, const std::string& value, bool last = false) {
        out += "  " + json_string(key) + ": " + value + (last ? "\n" : ",\n");
    };

    line("found", "true");
    line("soul", json_string(soul.soul));
    line("name", json_string(persona.name));
    // Which of the two written greetings was said. Reported rather than left to be inferred
    // because the app is the one that knows -- it reads the save's record of who has met whom -- and
    // because a caller holding a persona needs to be able to say what it was built for after
    // the fact.
    line("met", approach.met ? "true" : "false");
    // The prose the model is given, on its own as well as inside the sheet. It names every
    // state that applied, which saves the playground reimplementing the bands to explain
    // what the model was told.
    line("prose", json_string(approach_prose(approach, doing.clause)));
    // The world's two readings as the words they came to, for the same reason: the
    // playground has to label them without owning a second copy of the boundaries, and it was
    // a duplicated band that put "filthy" two rungs off the game's own value for a day. Empty
    // means nothing to say -- an unread clock, or a dry day, which is deliberate silence.
    line("daypart", json_string(std::string(part_of_day(approach.hour))));
    line("sky", json_string(std::string(sky(approach.rain))));
    // Both halves of the activity, because the clause is what the sheet says and the
    // activity is the category it was sorted under, and the playground shows each in a
    // different place.
    line("clause", json_string(doing.clause));
    line("activity", json_string(doing.activity));
    // Whether the soul was met away from every spot of their routine, which is what kept
    // back the remarks on the place; "unknown" when there was no position to set.
    line("away", approach.x == kUnread || approach.y == kUnread
                     ? "\"unknown\""
                     : (soul.spots.empty() ? "\"unknown\""
                                           : (away_from_places(soul, approach) ? "true" : "false")));

    // The system message in full, exactly as system_prompt() assembles it for a real turn.
    line("system", json_string(system_prompt(world, persona)));
    // And its two halves, so the playground can show which part is shared and therefore
    // already paid for, and which is charged again for every soul the player walks up to.
    line("prefix", json_string(shared_prefix(world)));
    line("sheet", json_string(persona.sheet));
    // And the prefix's own two halves, since one is the setting a player could change and the
    // other is the speaking rules that are true of every soul in the game.
    line("world", json_string(world));
    line("speaking", json_string(kSpeaking));

    // Where each block of the sheet came from, in the order they appear in it. The sheet is a
    // wall of prose by the time the model sees it, and which part of it is the written card,
    // which is the kin, and which is the state the player walked up in is not recoverable from
    // looking at it.
    out += "  \"parts\": [";
    for (std::size_t i = 0; i < persona.parts.size(); ++i) {
        out += i ? ",\n" : "\n";
        out += "    { \"label\": " + json_string(persona.parts[i].label) +
               ", \"text\": " + json_string(persona.parts[i].text) + " }";
    }
    out += persona.parts.empty() ? "],\n" : "\n  ],\n";

    line("greeting", json_string(persona.greeting));
    // Whether that line was written for the very visit (the giver's, on a quest turn), or is
    // the soul's general line standing in -- which is what decides whether the app asks the model for
    // a fresher one while the player is only looking at them (Session::look).
    line("greeting_exact", std::string(yes_no(persona.greeting_exact)));

    // The quest, as the director settled it for this conversation, or null. The playground
    // plays the game's part here: it keeps the state, sends the question to /api/judge with
    // the player's words, and on a yes records the command and adds the direction. Every
    // decision is the app's; the browser only carries them.
    if (persona.quest_of.exists()) {
        const Quest& quest = persona.quest_of;
        const QuestPlan& plan = persona.quest;
        QuestFacts told = facts;
        if (told.have < 0) told.have = approach.have;
        out += "  \"quest\": {\n";
        out += "    \"id\": " + json_string(quest.id) + ",\n";
        out += "    \"kind\": " + json_string(quest.kind) + ",\n";
        out += "    \"role\": " + json_string(quest.role == QuestRole::Giver    ? "giver"
                                              : quest.role == QuestRole::Onward ? "onward"
                                                                                : "target") +
               ",\n";
        // The giver's own opening for this visit, when the errand makes it one of its own
        // and the writer gave him a line; it is then the `greeting` above.
        out += "    \"visit_greeting\": " + json_string(plan.greeting) + ",\n";
        out += "    \"name\": " + json_string(quest.name) + ",\n";
        out += "    \"item_name\": " + json_string(quest.item_name) + ",\n";
        out += "    \"item_class\": " + json_string(quest.item_class) + ",\n";
        out += "    \"giver_name\": " + json_string(quest.giver_name) + ",\n";
        out += "    \"giver_soul\": " + json_string(quest.giver_soul) + ",\n";
        out += "    \"target_name\": " + json_string(quest.target_name) + ",\n";
        out += "    \"target_soul\": " + json_string(quest.target_soul) + ",\n";
        out += "    \"reward_groschen\": " + std::to_string(quest.reward_groschen) + ",\n";
        out += "    \"reward_text\": " + json_string(quest.reward_text) + ",\n";
        out += "    \"difficulty\": " + json_string(quest.difficulty) + ",\n";
        out += "    \"offer\": " + json_string(quest.offer) + ",\n";
        out += "    \"thanks\": " + json_string(quest.thanks) + ",\n";
        // The slate: a chain's link, a nest's inner favour, a dispute.
        out += "    \"chain_id\": " + json_string(quest.chain_id) + ",\n";
        out += "    \"chain_order\": " + std::to_string(quest.chain_order) + ",\n";
        out += "    \"after_quest_id\": " + json_string(quest.after_quest_id) + ",\n";
        out += "    \"requires_quest_id\": " + json_string(quest.requires_quest_id) + ",\n";
        out += "    \"dispute\": " + std::string(yes_no(quest.dispute)) + ",\n";
        out += "    \"facts\": { \"accepted\": " + std::string(yes_no(told.accepted)) +
               ", \"handed\": " + std::string(yes_no(told.handed)) +
               ", \"done\": " + std::string(yes_no(told.done)) +
               ", \"have\": " + std::to_string(told.have) +
               ", \"broker\": " + std::string(yes_no(told.broker)) +
               ", \"apologised\": " + std::string(yes_no(told.apologised)) +
               ", \"refused\": " + std::string(yes_no(told.refused)) +
               ", \"onward\": " + std::string(yes_no(told.onward)) +
               ", \"keepsake\": " + std::string(yes_no(told.keepsake)) +
               // The store's how, only where one is given, so a golden without one stands.
               (told.how.empty() ? std::string() : ", \"how\": " + json_string(told.how)) +
               // And a dictated letter's digits, the same way.
               (told.wrote.empty() ? std::string() : ", \"wrote\": " + json_string(told.wrote)) + " },\n";
        // The dispute's row and the giver's written answer, when the player carries the
        // holder's condition; empty otherwise.
        out += "    \"apologise_row\": " + json_string(plan.apologise_row) + ",\n";
        out += "    \"apologise_reply\": " + json_string(plan.apologise_reply) + ",\n";
        out += "    \"apologise_command\": " + json_string(plan.apologise_command) + ",\n";
        // The director's own rows (a deliver's report and keep), each with its answer.
        out += "    \"rows\": [";
        for (std::size_t i = 0; i < plan.rows.size(); ++i) {
            const DirectorRow& row = plan.rows[i];
            out += std::string(i ? ", " : "") + "{ \"intent\": " + json_string(row.intent) +
                   ", \"text\": " + json_string(row.text) + ", \"reply\": " + json_string(row.reply) +
                   ", \"command\": " + json_string(row.command) + " }";
        }
        out += "],\n";
        out += "    \"block\": " + json_string(plan.block) + ",\n";
        out += "    \"opening_commands\": [";
        for (std::size_t i = 0; i < plan.opening_commands.size(); ++i) {
            out += (i ? ", " : "") + json_string(plan.opening_commands[i]);
        }
        out += "],\n";
        out += "    \"question\": " + json_string(plan.question) + ",\n";
        out += "    \"on_yes\": " + json_string(plan.on_yes) + ",\n";
        out += "    \"direction\": " + json_string(plan.direction) + ",\n";
        // The lines a turn file would carry, so the playground can show exactly what the
        // mod would read.
        out += "    \"directives\": [";
        for (std::size_t i = 0; i < plan.opening_commands.size(); ++i) {
            out += (i ? ", " : "") + json_string(quest_directive(quest, plan.opening_commands[i]));
        }
        out += "]\n";
        out += "  },\n";
    } else {
        line("quest", "null");
    }

    // The menu's own answers as the director settled them for this conversation, so the
    // playground can say them as the app would rather than ask a model. The trust tier is
    // named so a topic's absence can be explained.
    {
        const Scripted& written = persona.scripted;
        out += "  \"scripted\": {\n";
        out += "    \"trust\": " + json_string(std::string(trust_word(trust_tier(standing, facts)))) + ",\n";
        // Whether a typed question is answered from what they know, or put off as the help
        // their favour pays for (KbGate).
        out += "    \"kb_gate\": " + json_string(std::string(kb_gate_word(written.kb_gate))) + ",\n";
        out += "    \"offer\": " + json_string(written.offer) + ",\n";
        out += "    \"help_row\": " + json_string(written.help_row) + ",\n";
        out += "    \"accept_row\": " + json_string(written.accept_row) + ",\n";
        out += "    \"accept_reply\": " + json_string(written.accept_reply) + ",\n";
        out += "    \"more_row\": " + json_string(written.more_row) + ",\n";
        out += "    \"more_reply\": " + json_string(written.more_reply) + ",\n";
        out += "    \"help\": " + json_string(written.help) + ",\n";
        out += "    \"about\": " + json_string(written.about) + ",\n";
        out += "    \"about_told\": " + std::string(yes_no(written.about_told)) + ",\n";
        out += "    \"withheld\": " + std::string(yes_no(written.withheld)) + ",\n";
        const std::vector<Topic> first_three(
            written.topics.begin(),
            written.topics.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(3, written.topics.size())));
        out += "    \"ask_reply\": " + json_string(first_three.empty() ? std::string() : ask_reply(first_three, 0)) + ",\n";
        out += "    \"topics\": [";
        for (std::size_t i = 0; i < written.topics.size(); ++i) {
            const Topic& topic = written.topics[i];
            out += i ? ",\n" : "\n";
            out += "      { \"key\": " + json_string(topic.key) + ", \"label\": " + json_string(topic.label) +
                   ", \"intent\": " + json_string(ask_intent(topic.key)) + ", \"row\": " + json_string(ask_row(topic)) +
                   ", \"text\": " + json_string(topic.text) + " }";
        }
        out += written.topics.empty() ? "],\n" : "\n    ],\n";
        // Henry's remarks with their answers, each a row on its own (say:<key>).
        out += "    \"remarks\": [";
        for (std::size_t i = 0; i < written.remarks.size(); ++i) {
            const Remark& remark = written.remarks[i];
            out += i ? ",\n" : "\n";
            out += "      { \"key\": " + json_string(remark.key) + ", \"intent\": " + json_string(say_intent(remark.key)) +
                   ", \"row\": " + json_string(remark.row) + ", \"text\": " + json_string(remark.text) + " }";
        }
        out += written.remarks.empty() ? "]\n" : "\n    ]\n";
        out += "  },\n";
    }

    // Whether written greetings are in use at all. Reported rather than inferred from an empty
    // greeting, because empty means two different things -- nothing was written for this soul, or
    // nothing written is being said -- and a caller showing the written lines needs to know which.
    line("say_written", say_written ? "true" : "false");
    // What stands in for the player's first turn when there is no written greeting, since
    // a chat template wants a user turn before the character can say anything.
    line("approached", json_string(kApproached));

    // Both written greetings, whichever was said, because the playground's reason for existing
    // is to show what a character has waiting to be said before a model is involved -- which
    // includes whether they recognise the player. Empty strings for a soul the pass has not
    // reached.
    out += "  \"greetings\": { \"first\": " + json_string(soul.greeting_first) +
           ", \"again\": " + json_string(soul.greeting_again) + " },\n";

    out += "  \"options\": {\n";
    out += "    \"model\": " + json_string(options.model) + ",\n";
    out += "    \"temperature\": " + number(options.temperature) + ",\n";
    out += "    \"top_p\": " + number(options.top_p) + ",\n";
    out += "    \"top_k\": " + std::to_string(options.top_k) + ",\n";
    out += "    \"context_tokens\": " + std::to_string(options.context_tokens) + ",\n";
    out += "    \"reply_tokens\": " + std::to_string(options.reply_tokens) + ",\n";
    out += "    \"disable_thinking\": " + std::string(yes_no(options.disable_thinking)) + ",\n";
    out += "    \"keep_alive\": " + json_string(options.keep_alive) + ",\n";
    out += "    \"buffer_chars\": " + std::to_string(least_chars) + "\n";
    out += "  },\n";

    // Why the sheet reads as it does. Not used to build anything; shown beside it, because
    // "where did that come from" is the question a bad sheet always raises.
    out += "  \"source\": {\n";
    out += "    \"occupation\": " + json_string(soul.occupation) + ",\n";
    out += "    \"trade\": " + json_string(soul.trade) + ",\n";
    out += "    \"social_class\": " + json_string(soul.social_class) + ",\n";
    out += "    \"settlement\": " + json_string(soul.settlement) + ",\n";
    out += "    \"region\": " + json_string(soul.region) + ",\n";
    out += "    \"gender\": " + json_string(soul.gender) + ",\n";
    out += "    \"card\": " + std::string(yes_no(!soul.card.empty())) + ",\n";
    out += "    \"bio\": " + std::string(yes_no(!soul.bio.empty())) + ",\n";
    out += "    \"kin\": " + std::to_string(soul.kin.size()) + ",\n";
    out += "    \"trouble\": " + json_string(soul.trouble) + "\n";
    out += "  }\n";

    out += "}\n";
    return out;
}

}  // namespace moonshine
