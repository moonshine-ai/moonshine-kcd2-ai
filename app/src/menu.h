// The reply menu: up to four things Henry might say next, picked from with the game's own
// dialogue list.
//
// Every row is written before the conversation: the design's three seeds on the opening
// round, the director's rows for the errand of the moment (quest.h), and what the database
// holds for this soul -- the accepting line, the things they know, Henry's remarks to them
// (scripted.h). Each is `<intent> | <words>`; the intent is one of a closed list the
// director understands. The rows go to the game as `@opt` rows on turn files, and the mod
// draws them over the game's list widget (see data/kcd2/conversation-design/plan.md,
// section 2). What the player picks comes back as `BRIDGE picked <row> <intent> -- <words>`,
// followed by the same words as a said line, so a pick is answered exactly as a typed line
// is, except that the director already knows what it meant and asks the judge nothing.
//
// The model writes no rows. It did once -- asked after every line for whatever would fill
// the four -- and a row it wrote was a row it then had to answer, so a list that comes up
// short now stays short, and an empty list has the player type.

#ifndef MOONSHINE_MENU_H
#define MOONSHINE_MENU_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine {

// Rows on a list at most. The mod adds "(End dialog)" under them itself.
inline constexpr int kMenuOptions = 4;

// What a row is written to do, and all a row can be written to do. The director acts on
// `accept` (the errand is taken up, no judge asked), puts `help` and `agree` to the judge
// after an offer, and treats everything else as words; the list is closed so that a row's
// meaning never depends on reading the words twice. The intents with an argument,
// `ask:<topic>` and `say:<remark>`, are the app's own (scripted.h). `more` is Henry's one
// written question under an offer (scripted.h, more_row), answered as written and leaving
// the offer open; `more_2` is his second, where one was written (quest.h, more_2).
inline constexpr std::string_view kIntents[] = {
    "help", "accept", "ask", "about", "agree", "decline", "press", "joke", "leave_soon", "farewell",
    "remind", "more", "more_2", "alms", "pry",
};
// Henry's row at a beggar, while he carries something the mod says he could spare
// (approach.h, alms); picked, it is answered with the rows for what he has -- `alms:money`,
// `alms:food`, `alms:clothes` (scripted.h) -- and each of those owes the mod `@alms <kind>`,
// which moves the thing and, for a garment, puts it on him.
inline constexpr std::string_view kAlmsRow = "I've something for you.";
// `pry` is Henry's row at a letter's writer asking what it says (scripted.h, pry_row),
// answered as written either way and weighed by the app against the two Speech stats.
// Henry's row at a giver whose errand he has taken up and not yet finished, in the place of
// the opening round's offer of help: he has already said he would, and "Is there anything I
// can help you with?" from a man carrying your purse's errand was the wrong question. The
// answer is the giver's own offer said again (scripted.h, `remind`).
inline constexpr std::string_view kRemindRow = "Remind me what it is you need of me.";
// The director's own, like `ask:<topic>`. `apologise` is Henry's row that puts a dispute
// holder's condition to the giver (quest.h), whose written answer owes the game `apologise`
// or `refuse_apology`. `report` and `keep` are a deliver's rows at the writer's: the letter
// is delivered (true or not), or Henry has not carried it and will not. `tell`, `lie` and
// `quiet` are a travel's; `fought` and `confess` a beat's and a mind's (quest.h). `give` and
// `lie_have` are a retrieve's with the thing in Henry's coat and a lie written for it: the
// hand-in as a row, and the lie that the holder never had it. `dictate` is Henry picking a
// dictated letter up with its writer (quest.h, kDictateIntent); his four ways with each
// part are `write:<k>`, `reconsider:<k>`, `soften:<k>`, `embellish:<k>` (dictation_part).
inline constexpr std::string_view kDirectorIntents[] = {"apologise", "refuse_apology", "report", "keep",
                                                        "tell", "lie", "quiet", "fought", "confess",
                                                        "lie_soft", "lie_big", "give", "lie_have",
                                                        "give_up", "accept_again", "dictate",
                                                        "fast_travel", "fast_travel_later"};
bool known_intent(std::string_view intent);

struct MenuOption {
    int row = 0;  // 1-based, in the order the rows were accepted
    std::string intent;
    std::string text;
};

// The three the design names for the opening round, in this order. They go to the game the
// moment the NPC has spoken. Asked to reword them, a 4B model turned them round into the
// NPC's own words, and a menu that appears at once is worth more than three lines with
// local colour.
inline constexpr std::string_view kFirstRoundSeeds[] = {
    "Is there anything I can help you with?",
    "I have a question.",
    "Tell me about yourself.",
};
std::vector<MenuOption> first_round_rows();

// The rows as a turn file carries them: "@opt <row> <intent> <bytes>:<words>", every row so
// far on one line, since a turn file is whole each time it is written.
std::string opt_directive(const MenuOption& option);
std::string opt_turn(const std::vector<MenuOption>& options);
// "@rows <n>": the list is whole at n rows (0 for none), so the mod draws it as it stands
// rather than waiting a grace for rows that are not coming, and draws a short list short.
// "@rows <n> closed": the same, and the list is these rows alone -- the mod leaves its own
// "(Say something else)" and "(End dialog)" off it, so a choice that has to be made (a
// dictated letter's part, notes.md 20 Oct 2026) is made from the rows written for it.
std::string rows_directive(std::size_t count, bool closed = false);
// The whole list as one turn: every row, then the count. Since 4 Oct 2026 a round's rows
// go on one file, decided together, since nothing on them waits on the model.
std::string whole_list_turn(const std::vector<MenuOption>& options, bool closed = false);

// The pick as the mod logs it: "BRIDGE picked <row> <intent> -- <words>". Nothing for a
// line that is not one.
struct Pick {
    int row = 0;
    std::string intent;
    std::string text;
};
std::optional<Pick> read_pick(std::string_view line);

}  // namespace moonshine

#endif  // MOONSHINE_MENU_H
