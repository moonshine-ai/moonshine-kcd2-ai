#include "prompt.h"

namespace moonshine {

namespace {

// The join between the world text and a character sheet. Fixed, because it is
// part of the prefix every NPC shares: varying it per NPC would move the point
// where two prompts diverge back to the start of the system message.
constexpr std::string_view kJoin = "\n\n";

}  // namespace

const std::string_view kApproached =
    "[Someone you do not know has walked up and is standing in front of you, "
    "waiting for you to speak. Say the one or two lines you would greet them with.]";

const std::string_view kSpeaking =
    "What follows is who you are, one person living in this world. Some of it is written "
    "as a note about you by name rather than addressed to you; that is still you.\n"
    "\n"
    "You are speaking aloud to someone standing in front of you. Reply only with the "
    "words you say, in two or three sentences at most. Never describe what you do, "
    "never write your own name before your words, never say again a line you have already "
    "said, and never break character. Never say a number aloud, not a count of things and "
    "not a sum of money: it is a few, a good bunch, a fair price, what was promised. "
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
    messages.push_back({"user", said.empty() ? std::string(kApproached) : std::string(said)});
    return messages;
}

}  // namespace moonshine
