// Assembling a prompt so that the shared part of it stays cached.
//
// The world setting is most of the prompt and is identical for every NPC in the
// game, and a backend reuses a prefix only when it matches byte for byte. Putting
// the world first means a second NPC's conversation prefills their character
// sheet and nothing else: measured on qwen3:4b-instruct, 0.85s against 6.55s
// cold. Putting the character sheet first shares nothing, and measured no saving
// whatever, on every model tried.
//
// The consequence is a rule this header exists to enforce: nothing that varies
// between conversations may appear before the player's turn. No timestamp, no
// conversation id, no turn counter. See benchmarks/README.md.

#ifndef MOONSHINE_PROMPT_H
#define MOONSHINE_PROMPT_H

#include <string>
#include <string_view>
#include <vector>

#include "llm.h"
#include "quest.h"
#include "scripted.h"

namespace moonshine {

struct Persona {
    // For the subtitle and the log. Deliberately not used to build the prefix,
    // because a name placed before the world text would break the sharing.
    std::string name;
    // What the model is told about them. Every token here is paid for on the
    // opening line of a conversation, measured at about 29ms per token on a 7B
    // and 15ms on a 3B, so this is the text worth keeping short.
    std::string sheet;
    // What they say first, written ahead of time and said as it stands. Empty when
    // nothing was written for this soul, in which case the opening line is generated
    // like any other -- which is what the first real conversation did, spending 15.15s
    // before a word was spoken.
    std::string greeting;
    // Whether that greeting was written for this very visit -- the giver's line for the turn
    // the axe comes back or the favour is asked after -- rather than being the soul's
    // general line for a stranger or an acquaintance, which knows nothing of the state the
    // player walks up in. What decides whether the model is asked for a fresher one while
    // the player is only looking (Session::look).
    bool greeting_exact = false;

    // Which recording speaks them: pete-female or pete-male, until the actors' rights
    // are cleared. Kept off the sheet on purpose. After the greeting, not before the
    // sheet: the tests and main.cpp's fallback build a Persona as {name, sheet, greeting},
    // and a field placed between those took the sheet for a voice name and left the sheet
    // holding the greeting.
    std::string actor;
    // Whose shipped facial clips move their mouth while a line is spoken: the game's own
    // actor code for the soul (voice.xml voice_id, actors.txt), which is game data and
    // not a recording. Empty for a soul with no voice id; anybody's face then (lipsync.h).
    std::string face;

    // What the player walked up on them doing, in words: "sawing wood". Already part of
    // the sheet, and carried separately so the status line can say it without asking the
    // database again -- the one fact about a conversation that differs between two
    // conversations with the same soul, and so the first thing worth having in the log
    // when an answer comes out ignoring what the NPC was plainly in the middle of.
    // Empty when nothing was caught.
    std::string doing;

    // The blocks the sheet is made of, in the order they appear in it, each labelled with
    // where it came from: "who", "card" or "bio", "kin", "hook", "approach".
    //
    // Recorded as they are appended rather than recovered afterwards by splitting the sheet
    // on blank lines, which would need a second copy of the order to label them by and would
    // be wrong the first time a block contained a blank line of its own. Every block in here
    // is a decision -- the card in preference to the bio, the kin cut to four, the approach
    // last so everything above it can be prefilled -- and none of them is visible in the
    // finished sheet, which is why answering "why is the model being told that" used to mean
    // reading this function.
    struct Part {
        std::string label;
        std::string text;
    };
    std::vector<Part> parts;

    // What the director decided about this soul's quest for this conversation: the paragraph
    // already in the sheet under the "quest" label, the commands owed to the game before a
    // word is said, and the question to put to the model about the player's words. Empty
    // throughout for a soul with no quest, which is most of them.
    QuestPlan quest;
    // And the quest itself, for the directive lines and the log.
    Quest quest_of;
    // What the menu's own rows are answered with when they are picked, decided when the
    // conversation opens like the quest plan (scripted.h). Empty throughout for a soul
    // nobody wrote them for, whose picks the model answers as it always did.
    Scripted scripted;
};

// How a voiced NPC is to speak. True of every soul in the game, so it belongs in the
// shared prefix and not in anybody's character sheet: put in the sheet it is prefilled
// again for every NPC the player meets, and at roughly 15ms a token on a 4B that is
// paid on each opening line for text that never changes.
//
// Sheets address the character in the second person -- "You are Bartel, a varlet" -- so
// there is nothing to translate before acting on them. The one exception is a soul with no
// written card, whose trouble comes from a pre-baked hook written in the third person and
// using their name; converting those would mean conjugating the verb in 3,665 of them, and
// getting that wrong reads worse than not trying. Hence the second sentence here, which
// exists to cover exactly that block.
extern const std::string_view kSpeaking;

// What every NPC's system message begins with, and therefore what the backend is
// being asked to keep: the world, then the rules above. Exposed so a test can assert
// the sharing rather than trust it, and so the app can prefill it at start-up.
std::string shared_prefix(std::string_view world);

// The system message for one NPC: the shared prefix, then their sheet.
std::string system_prompt(std::string_view world, const Persona& persona);

// What the player's turn says when they have not said anything yet: the NPC speaks
// first, so something has to tell the model that. Left empty, the model is handed a
// blank turn and free-associates from the character sheet -- measured, an NPC opened
// with "Bread's good today, sister", addressing their sister rather than the player
// stood in front of them. Written as a direction rather than as speech so there is
// nothing in it to answer.
extern const std::string_view kApproached;

// A conversation ready to send. `history` alternates player and NPC turns, oldest
// first, and `said` is what the player has just said. Empty means they have not
// spoken yet and this is the opening line, which is prompted with kApproached.
std::vector<Message> conversation(std::string_view world,
                                  const Persona& persona,
                                  const std::vector<Message>& history,
                                  std::string_view said);

}  // namespace moonshine

#endif  // MOONSHINE_PROMPT_H
