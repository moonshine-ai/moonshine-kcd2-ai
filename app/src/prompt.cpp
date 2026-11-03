#include "prompt.h"

namespace moonshine {

namespace {

// The join between the world text and a character sheet. Fixed, because it is
// part of the prefix every NPC shares: varying it per NPC would move the point
// where two prompts diverge back to the start of the system message.
constexpr std::string_view kJoin = "\n\n";

}  // namespace

// The one who walks up is always Henry, a young man: said so, since a bare "someone" had
// Christine greet him "Welcome, mistress" (bugs.txt 2 Nov 2026). And whether they have met:
// the stranger's version was the only one, and was sent for a soul met a dozen times and
// owed a favour, which is the lapse the reunion greetings exist to avoid. The man they have
// met is named: told only that a young man they knew stood there, and that he looked like a
// noble, Christine greeted him "Herr von Bergow", the one noble house the setting names
// (bugs.txt 3 Nov 2026).
const std::string_view kApproached =
    "[A young man you do not know has walked up and is standing in front of you, "
    "waiting for you to speak. Say the one or two lines you would greet him with.]";
const std::string_view kApproachedAgain =
    "[Henry, the young man you have spoken to before, has walked up and is standing in "
    "front of you, waiting for you to speak. Say the one or two lines you would greet him "
    "with, as somebody who knows him and remembers what has passed between you.]";

const std::string_view kSpeaking =
    "What follows is who you are, one person living in this world. Some of it is written "
    "as a note about you by name rather than addressed to you; that is still you.\n"
    "\n"
    "You are speaking aloud to someone standing in front of you. Reply only with the "
    "words you say, in two or three sentences at most. Never describe what you do, "
    "never write your own name before your words, never say again a line you have already "
    "said, and never break character. Never say a number aloud, not a count of things and "
    "not a sum of money: it is a few, a good bunch, a fair price, what was promised. "
    "The only coin is the groschen; never speak of hellers, pennies or any other. "
    "Call the one you are speaking to by a name or a title only if you have been told it; "
    "never invent one for him. "
    "Write no dashes; a full stop or a comma does their work.\n"
    "\n"
    "Sometimes a note in square brackets follows what they say. They did not say it and "
    "cannot see it: never repeat it, quote it or refer to it. It tells you what has just "
    "become true, or what you know of the thing they asked, or that you know nothing of it, "
    "or will not say yet. Use what it gives only if it answers them, in your own words and "
    "with every particular kept; if it does not answer them, say you cannot help with that, "
    "and invent no fact, name or place to fill the gap.";

std::string shared_prefix(std::string_view world) {
    std::string prefix;
    prefix.reserve(world.size() + kSpeaking.size() + 2 * kJoin.size());
    prefix.append(world);
    prefix.append(kJoin);
    prefix.append(kSpeaking);
    prefix.append(kJoin);
    return prefix;
}

std::string system_prompt(std::string_view world, const Persona& persona) {
    std::string text = shared_prefix(world);
    text.append(persona.sheet);
    return text;
}

std::vector<Message> conversation(std::string_view world,
                                  const Persona& persona,
                                  const std::vector<Message>& history,
                                  std::string_view said) {
    std::vector<Message> messages;
    messages.reserve(history.size() + 2);
    messages.push_back({"system", system_prompt(world, persona)});
    // History is appended unchanged. Rewriting an earlier turn, even to tidy it,
    // would invalidate the cached prefix from that turn onward and turn a warm
    // line back into a cold one.
    messages.insert(messages.end(), history.begin(), history.end());
    messages.push_back({"user", !said.empty()  ? std::string(said)
                                : persona.met ? std::string(kApproachedAgain)
                                              : std::string(kApproached)});
    return messages;
}

}  // namespace moonshine
