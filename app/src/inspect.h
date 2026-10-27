// How the app describes a conversation it has not had yet, for another process to read.
//
// The playground stands in for the game while the voice is being built: it picks a soul,
// says how the player looks, and talks to the model over the same prompt the game would
// produce. For that to be worth anything the prompt has to be this app's, not a second
// implementation of it in JavaScript -- a playground that assembles its own system message
// is testing itself. So the app hands the whole thing over, already built, and the
// playground only adds the turns.

#ifndef MOONSHINE_INSPECT_H
#define MOONSHINE_INSPECT_H

#include <cstddef>
#include <string>
#include <string_view>

#include "llm.h"
#include "personadb.h"

namespace moonshine {

// Everything needed to put the same words to a model that a real turn would: the system
// message in full, the greeting that is said without asking a model at all, and the
// sampling to ask for. Plus what the sheet was built from, which is the first question
// anyone asks when a sheet reads badly.
// `least_chars` is the app's speech buffer: how much of a reply is held back before the
// voice starts on it. Handed over because the playground reports when audio would begin,
// and that answer is meaningless if the two disagree about the threshold.
// `facts` is where the soul's quest stands, as the playground says it does; no save is
// consulted here, since the playground is the one keeping score.
// `memory` is the sheet's memory block, already in words, or empty for a first meeting.
std::string persona_json(const Soul& soul,
                         std::string_view world,
                         const Approach& approach,
                         const Doing& doing,
                         const Options& options,
                         std::size_t least_chars,
                         bool say_written = true,
                         const QuestFacts& facts = {},
                         const std::string& memory = {},
                         const Standing& standing = {});

// One JSON string literal, escapes and surrounding quotes included.
//
// Deliberately not called `quoted`. std::quoted is a stream manipulator that takes a
// std::string_view, so argument-dependent lookup finds it from a call like `quoted(sheet)`
// and it wins: it wraps the text in quotes and escapes the quotes, which looks correct
// until a character sheet with a paragraph break in it goes out with a raw newline in the
// middle of a JSON string.
std::string json_string(std::string_view text);

}  // namespace moonshine

#endif  // MOONSHINE_INSPECT_H
