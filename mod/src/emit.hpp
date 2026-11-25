#pragma once

#include <string>
#include <utility>
#include <vector>

#include "npcdb.hpp"

namespace kcd2 {

// Localization key for a soul's generated name. Namespaced by modid so it can
// never collide with a shipped stringId.
std::string ui_name_key(const std::string& modid, const std::string& soul_name);

// Localization key for the interactor hint on the conversation option.
std::string conversation_hint_key(const std::string& modid);
// The localization key for the prompt shown on a giver whose favour is still to be taken up.
std::string favour_hint_key(const std::string& modid);
// The localization key and the word for the action hint under the menu that the talk
// button can be held (SayHint in the Lua).
std::string say_hint_key(const std::string& modid);
std::string say_hint_text();

// Data/libs/Storm/storm__<modid>.xml -- merged into the game's storm root to
// add our rules files as sources of the existing tasks. The roles task is only
// declared when with_roles, since it is what makes souls talkable and that is
// opt-in.
std::string emit_storm_root(const std::string& modid, bool with_roles);

// Data/libs/Tables/rpg/metarole__<modid>.xml -- declares the group our role
// belongs to. Worth its own row because a metarole is the only handle Lua has:
// soul:AddMetaRoleByName is the one runtime grant that exists, so a soul a save
// created before this mod ran can only be reached through one of these.
// The quests add a metarole each, for the holder's own dialogue to speak through.
std::string emit_metarole_table(const std::string& modid, const std::string& metarole,
                                const std::vector<Quest>& quests,
                                bool with_guards = false);

// Data/libs/Tables/rpg/role__<modid>.xml -- declares the role before anything
// grants it or speaks through it. Unlike a setUiName key, a role is not a
// free-form identifier: <addRole> and a dialogue's Role attribute both resolve
// against this table, so one that is missing from it silently matches nothing.
// An additive fragment rather than a patch of the 745KB original, which is how
// the shipped tables split themselves 981 times over.
std::string emit_role_table(const std::string& modid,
                            const std::string& role,
                            const std::string& metarole,
                            const std::vector<Quest>& quests,
                            bool with_guards = false);

// Data/libs/Storm/roles/<modid>.xml -- grants each soul the role our dialogue
// speaks through, which has to be one emit_role_table declared, and each quest's
// holder that quest's role.
std::string emit_storm_roles(const std::string& modid,
                             const std::vector<Persona>& personas,
                             const std::vector<std::string>& only_souls,
                             const std::string& role,
                             const std::vector<Quest>& quests,
                             std::size_t* granted,
                             const std::vector<GuardSoul>& guards = {});

// Guard town-directory role, metarole, dialogue node, and root menu prompt.
std::string guard_role(const std::string& modid);
std::string guard_metarole(const std::string& modid);
std::string guard_dialogue_node();
std::string guard_find_prompt_key(const std::string& modid);
std::string guard_find_prompt_text();

// The role a quest's holder dialogue speaks through, the metarole that role sits
// under (which is what the Lua can grant at runtime), and the node and file name of
// the dialogue: MSQ_<ID>_HOLDER, MSQ_<ID>, q_<id>_holder.
std::string holder_role(const std::string& quest_id);
std::string holder_metarole(const std::string& quest_id);
std::string holder_node(const std::string& quest_id);

// A pray's token: the item class the Lua makes in the player's pocket for a moment when
// Henry has knelt, and the graph's ItemDescriptorTrigger fires on. A guid of the mod's own
// from the quest id, and the item row's name (token_q_<id>).
std::string token_class(const Quest& quest);
// A duel's second token: the man cowed (emit_leg's cowed trigger; WatchDuel).
std::string cowed_class(const Quest& quest);
std::string cowed_name(const Quest& quest);
// A duel's third, where the man has a taunt: the men made and the brawl to begin
// (emit_leg's sprung trigger, emit_brawl's start; DuelLoose).
std::string sprung_class(const Quest& quest);
std::string sprung_name(const Quest& quest);
// A duel's fourth: taken up at the holder's door of an earlier scene (npcdb.hpp
// duel_quest_id; the Lua's QuestDoorDuel), for the duel node's doorAccepted trigger, which
// sets the quest Active as the talk's accept port would.
std::string door_accept_class(const Quest& quest);
std::string door_accept_name(const Quest& quest);
// A duel's fifth: the carry begun, the man's day the yard from here (Quest::has_yard;
// emit_leg's placed trigger, the Lua's FastTravelGo).
std::string yard_class(const Quest& quest);
std::string yard_name(const Quest& quest);
// A deliver's promised warning (npcdb.hpp warn_soul): the token on the app's `warn` that
// opens the optional objective on the one to be warned, and the one that closes it once
// the scene opening on the warning is taken up or refused at him (QuestCommand accept).
std::string warn_class(const Quest& quest);
std::string warn_name(const Quest& quest);
std::string warned_off_class(const Quest& quest);
std::string warned_off_name(const Quest& quest);
std::string token_name(const Quest& quest);
// A chest's second token: made when the thing has left the chest for Henry's pocket
// (WatchChest), for the trigger that completes the fetch objective (taken_q_<id>).
std::string taken_class(const Quest& quest);
std::string taken_name(const Quest& quest);
// A chest's paper: the Document laid beside the thing (paper_q_<id>), and its page's
// localisation key.
std::string paper_class(const Quest& quest);
std::string paper_name(const Quest& quest);
// The paper's read token (Quest::has_paper_read): made in Henry's pocket by the graph as he
// closes the paper (UseBookTrigger.OnClosed -> CreateItems), the one word the graph has for
// the Lua, which finds it on the watch and says his line (read_q_<id>).
std::string read_class(const Quest& quest);
std::string read_name(const Quest& quest);
// A dictated letter's Documents (Quest::has_dictation): one class per combination of the
// parts' outcomes, the three digits spelt into the guid between a fixed prefix and a
// suffix from the quest id, so the Lua builds the class from the store's digits with no
// table (LetterClass). The item row's name is the letter's with the digits appended.
std::string letter_class_prefix();
std::string letter_class_suffix(const Quest& quest);
std::string letter_class(const Quest& quest, const std::string& wrote);
std::string letter_variant_name(const Quest& quest, const std::string& wrote);
std::string paper_page_key(const std::string& modid, const Quest& quest);
// The counsel's notes (Quest::has_counsel; bugs.txt 19 Nov 2026): the Document the
// counsel gives Henry at the end of their answer, whichever way he takes out of it
// (notes_q_<id>), and its page's localisation key.
std::string counsel_notes_class(const Quest& quest);
std::string counsel_notes_name(const Quest& quest);
std::string counsel_notes_page_key(const std::string& modid, const Quest& quest);

// Data/Quests/<modid>/q_<id>_holder.xml -- the holder's side of a quest as the game's
// own dialogue menu, offered on them while the quest is Active and the thing has not
// gone: Henry says what he came about, the holder makes an excuse, and the menu offers
// the reasonable way (a speech check at the quest's tier), the hard way (a dread check
// at the same tier), money (if Henry has it), and leaving. A won branch fires the
// dialogue's handover port and runs Moonshine.QuestHolderGave on exit, which is where
// the item moves. Words come from npc_quest_lines, with plain fallbacks.
std::string emit_holder_dialogue(const std::string& modid, const Quest& quest);

// The twelve rows that dialogue reads its words from.
std::vector<std::pair<std::string, std::string>> holder_localization(const std::string& modid,
                                                                     const Quest& quest);

// Data/Quests/<modid>/q_<id>_followed.xml -- the holder turning on Henry in the street
// (npcdb.hpp has_follow), as the game's own forced dialogue: a ForcedDialog, the element 812
// shipped quest files use for an NPC who stops the player and speaks first, enqueued in the
// leg's graph by the followed token (emit_leg) with the holder as the one participant
// beside the player and the two turned to each other. His `followed` line and Henry's
// `followed_reply`, framed line by line as the holder's menu is, the same two text rows; the
// exit is the Talk's own (`open followed`: the row noted, Henry's word on the night).
// Chat 16 Nov 2026: "I didn't get a focused camera view when Ernest caught me watching him",
// then "can you try the ForceDialog call please, since that would offer a more standard
// experience". The Lua bind of that name (DialogModule.ForceDialog) is not it: either way
// round it opens a letterboxed chooser of raw sequence ids -- the open-world surrender and
// arrest dialogues, by their internal names -- with the holder walking on through it and the
// game waiting for a pick (follow_force_probe.lua, twice on wl1). The graph node is how the
// game itself forces a dialogue.
std::string followed_node(const std::string& quest_id);
std::string emit_followed_dialogue(const std::string& modid, const Quest& quest);
// A duel's taunt, the same shape (npcdb.hpp duel_taunt): the man's word as Henry lands,
// forced on him by the graph off the spring token, its ExitScript handing the Lua the
// moment to make the men and loose the brawl (DuelTaunted). Spoken through the duel's own
// role (taunt_role), granted to the man as the holder role is to a holder.
std::string taunt_node(const std::string& quest_id);
std::string taunt_role(const std::string& quest_id);
std::string taunt_metarole(const std::string& quest_id);
std::string emit_taunt_dialogue(const std::string& modid, const Quest& quest);
// Its text rows (taunt, taunt_reply), for the localization pak.
std::vector<std::pair<std::string, std::string>> taunt_localization(const std::string& modid, const Quest& quest);

// One beat of the dialogue graph that exists to fire a quest port. The app decides
// when -- it writes "@quest <id> <command>" ahead of a turn's words -- and the Lua
// turns that into the code below, which the graph Autoselects a sequence on, the
// same way it picks a face. Codes start at 1 because 0 is "no quest beat wanted".
struct QuestBeat {
    std::string quest_id;
    std::string command;  // "accept" or "complete"
    int code = 0;
};

// The graph-side commands of one quest, in code order: accept, complete, and a
// complete_<how> for each way the journal has its own completed log for
// (Quest::resolutions). handover is not among them: moving the item from the holder to
// the player is done in Lua, where the inventories are, and needs nothing from the graph.
std::vector<std::string> quest_commands(const Quest& quest);
std::vector<QuestBeat> quest_beats(const std::vector<Quest>& quests);

// The dialogue global the Lua keeps for one fact of a quest that the holder's menu
// reads: <modid>_q_<id>_<flag>. "inner" is 1 once the favour this one requires is done;
// "broker" is 1 once Henry has offered to carry a dispute holder's grievance.
std::string quest_flag_variable(const std::string& modid, const std::string& quest_id,
                                const std::string& flag);

// The objective's own state type, MSQ_<ID>_FETCH, with a Completed enumeration per
// resolution named as resolution_state spells it ("persuaded" -> "Persuaded").
std::string objective_type(const std::string& quest_id);
std::string resolution_state(const std::string& how);

// The second objective, on the kinds with a moment in the middle that sends Henry back
// to the giver (an ask's telling, a travel's arrival, a beat's man down, a prayer said, a
// thing put down, a mind's last visit): its name in the graph, whether the quest has one,
// and the line the journal shows for it, "Return to <giver>".
constexpr const char* kReturnObjective = "return_to_giver";
bool quest_returns(const Quest& quest);
std::string return_objective(const Quest& quest);

// The third objective, on an ask whose word is carried on (Quest::carries_on): opened by
// the `onward` port when the giver has been told and has sent Henry on, with the marker on
// the one sent to, and done with the quest. Its line is the generator's objective_onward.
constexpr const char* kOnwardObjective = "tell_onward";
// The objective an ask with a fall (Quest::has_fall) opens on the holder's handover -- the
// yes with its condition is the morning's meeting arranged (bugs.txt 22 Nov 2026) -- with
// the marker on the giver, in place of a return, and done with the quest. Its line is the
// writer's objective_arranged; it has no Started log of its own (the fetch's log_told is
// the journal's word).
constexpr const char* kArrangedObjective = "meet_giver";
// The hour of the game's day the one asked falls at: this hour of the calendar day after
// the yes (StateQuest handover; WatchAsk), so the meeting is never the same day's.
constexpr int kFallHour = 6;
// The onward soul's own dialogue: the one `hand` sequence of a word deliver's menu, under
// a role of its own (onward_role) so only they offer it, as q_<id>_onward.xml beside the
// holder's.
std::string onward_metarole(const std::string& quest_id);
std::string onward_role(const std::string& quest_id);
std::string onward_node(const std::string& quest_id);
std::string emit_onward_dialogue(const std::string& modid, const Quest& quest);
std::vector<std::pair<std::string, std::string>> onward_localization(const std::string& modid,
                                                                     const Quest& quest);

// The optional objective on an unyielding retrieve with a helper (Quest::has_helper; In
// the Stocks' Elsbeth): opened by the naming token when the giver has named the one who
// can let Henry in, with the marker on her, IsOptional, done with the quest. Its line is
// the generator's objective_helper. Her own dialogue is q_<id>_helper.xml under a role of
// her own (helper_role): the door row (for groschen; the `door` Out port opens the holder's
// house), the key row (a Speech check and groschen; the `key` Out port, the key class made
// in Henry's pocket) and the way out. The key is a MiscItem row of the mod's own
// (key_class) that the Lua's chest is keyed to.
constexpr const char* kHelperObjective = "speak_helper";
// The counsel's optional objective (Blind Pride's Brother Thomas): "Ask <counsel> for the
// argument", marker on them from the accept until asked.
constexpr const char* kCounselObjective = "ask_counsel";
// The warning promised (npcdb.hpp warn_soul; bugs.txt 23 Nov 2026): "Find <name> and warn
// him off", marker on him from the promise until he has had it or the errand is over.
constexpr const char* kWarnObjective = "warn_off";
std::string helper_metarole(const std::string& quest_id);
std::string helper_role(const std::string& quest_id);
std::string helper_node(const std::string& quest_id);
// The one with the argument (Blind Pride's Brother Thomas): their metarole, role and the
// Talk node their menu is written to (q_<id>_counsel.xml).
std::string counsel_metarole(const std::string& quest_id);
std::string counsel_role(const std::string& quest_id);
std::string counsel_node(const std::string& quest_id);
std::string key_class(const Quest& quest);
std::string key_name(const Quest& quest);
std::string emit_helper_dialogue(const std::string& modid, const Quest& quest);
// The counsel's menu (Blind Pride's Brother Thomas) and its strings.
std::string emit_counsel_dialogue(const std::string& modid, const Quest& quest);
std::vector<std::pair<std::string, std::string>> counsel_localization(const std::string& modid,
                                                                      const Quest& quest);
std::vector<std::pair<std::string, std::string>> helper_localization(const std::string& modid,
                                                                     const Quest& quest);

// A seek's clue-holders (Quest::clues): one optional objective each, ask_<i> (i from 1),
// "Ask <name> about the <animal>", marked on them from the accept until Henry has asked or
// the animal is found. The asking is the app's: the clue topic picked at them is said with
// `@quest <id> clue`, and the Lua makes a token of that holder's class (clue_class, the
// pray's shape under 41 -- the letter A -- with the holder's index in the second byte) for
// the graph's clue<i> trigger, which closes the objective (bugs.txt 9 Nov 2026).
std::string clue_class(const Quest& quest, std::size_t index);
std::string clue_name(const Quest& quest, std::size_t index);
std::string clue_objective(std::size_t index);
std::string clue_objective_text(const Quest& quest, std::size_t index);
// A holder's marker off while she stands where Henry may not go, and back when she comes
// out (bugs.txt 9 Nov 2026: "Lene was indoors in a private area when I found her"): the
// giver gate's rule (WatchGiver's Trespass) read under each holder by the Lua's WatchClues,
// and the street night's shape on her objective, whose type gains the marker-only states
// Shown and Hidden (clue_type) flipped by two tokens of her own, 53<ii> (S) and 68<ii> (h).
std::string clue_shown_class(const Quest& quest, std::size_t index);
std::string clue_shown_name(const Quest& quest, std::size_t index);
std::string clue_hidden_class(const Quest& quest, std::size_t index);
std::string clue_hidden_name(const Quest& quest, std::size_t index);
std::string clue_type(const std::string& quest_id, std::size_t index);
// The place to search (Quest::has_search; bugs.txt 9 Nov 2026: "Once one of the NPCs has
// mentioned the hen yards, a marker for that area should appear on the map"): the `search`
// objective, "Search <clue_text>", opened by the token of the search class (73, the letter
// s) the Lua makes as the first holder is asked, closed by the find; its marker an
// ItemOnGroundAsset of the search mark's class (kSearchMarkClass, a thing of the mod's own
// in the item table: uuid5 of "moonshine:item:sucked_egg", the custom items' rule), which the Lua lays at the animal's first point as the token is made
// (LaySearchMark) -- where she is made and calls as Henry comes within seekCallMetres, so
// the pin is where she is seen and heard -- and again from the watch if that failed.
std::string search_class(const Quest& quest);
std::string search_name(const Quest& quest);
std::string search_objective_text(const Quest& quest);
constexpr const char* kSearchObjective = "search";
constexpr const char* kSearchMarkClass = "86f47744-0555-564b-a32d-56ce873bb67c";
constexpr const char* kSearchMarkName = "moonshine_sucked_egg";

// Freeform NPC locate command (@locate <soul> <len>:<spots>): sharded per-level micro
// quests (q_locate_<level>_<shard>.xml, up to kLocateShardSize souls each) where each
// soul has its own SoulAsset marker and named objective ("Find <full_name>"). A 2D
// shard x slot token mux (1 shard-open token + 1 of 64 shared slot tokens) opens the
// target soul's objective, and kLocateClearClass resets both the objective and questState
// to None (SetNone) when Henry reaches/talks to the soul, asks for new directions, or
// after kLocateExpireHours in-game hours -- ensuring at most one directions quest and
// marker exists at any time, with zero ground items ever placed.
// Legacy kLocatePinClass / kLocateShownClass / kLocateHiddenClass and q_locate_<level>.xml
// remain as inert save-compatibility shims for saves made before sharding.
constexpr const char* kLocatePinClass = "6d6f6f6e-7368-6c70-8000-000000000001";
constexpr const char* kLocatePinName = "locate_pin";
constexpr const char* kLocateShownClass = "6d6f6f6e-7368-6c73-8000-000000000001";
constexpr const char* kLocateShownName = "locate_shown";
constexpr const char* kLocateHiddenClass = "6d6f6f6e-7368-6c68-8000-000000000001";
constexpr const char* kLocateHiddenName = "locate_hidden";
constexpr const char* kLocateClearClass = "6d6f6f6e-7368-6c63-8000-000000000001";
constexpr const char* kLocateClearName = "locate_clear";
constexpr std::size_t kLocateShardSize = 64;
constexpr double kLocateReachMetres = 6.0;
constexpr double kLocateExpireHours = 24.0;

int locate_level_code(const std::string& level);
std::string locate_slot_class(std::size_t slot);
std::string locate_slot_name(std::size_t slot);
std::string locate_shard_class(const std::string& level, std::size_t shard);
std::string locate_shard_name(const std::string& level, std::size_t shard);
std::string locate_shard_node(const std::string& level, std::size_t shard);
std::vector<const LocateSoul*> locate_souls_in_level(const std::vector<LocateSoul>& locate_souls,
                                                     const std::string& level);
std::size_t locate_shard_count(const std::vector<LocateSoul>& locate_souls, const std::string& level);

std::string locate_node(const std::string& level);
std::string emit_locate_quest(const std::string& modid, const std::string& level);
std::string emit_locate_shard_quest(const std::string& modid,
                                    const std::string& level,
                                    std::size_t shard,
                                    const std::vector<const LocateSoul*>& shard_souls);
std::vector<std::pair<std::string, std::string>> locate_localization(const std::string& modid,
                                                                     const std::string& level);
std::vector<std::pair<std::string, std::string>> locate_souls_localization(
    const std::string& modid, const std::vector<LocateSoul>& locate_souls);

// A giver's hail as the game's own bark (notes.md 14 Oct 2026, In the Stocks: Benesh's
// words over his head, not on the dialogue's caption). q_<id>_hail.xml is a one-line
// `ingame monolog` dialogue on the giver -- the shape ambient-design/plan.md section 1.1
// proved on the Windows laptop -- under the dice-game role every ordinary soul holds (KOSTKAR_MUZ /
// KOSTKAR_ZENA by the giver's gender), its decision aliased so the graph's RequestMonologue
// can name it. The watch decides when (WatchHail) and tells the graph by a token of the
// hail's class (hail_class) in Henry's pocket for a moment, the pray's channel; the graph's
// ItemDescriptorTrigger on it, live while the quest is unasked, requests the monologue.
// The request carries no ForceSubtitles, as the shipped barks' do not (a market seller's
// shout, a guard's remark): that flag is the behaviour trees' `subtitlesDown`, the line at
// the bottom of the screen in place of the bubble over the speaker's head (bark probe,
// 15 Nov 2026).
std::string hail_class(const Quest& quest);
std::string hail_name(const Quest& quest);
std::string hail_node(const std::string& quest_id);
std::string hail_alias(const std::string& modid, const std::string& quest_id);
std::string emit_hail_dialogue(const std::string& modid, const Quest& quest);

// Henry talking to himself, as the game has him do it (chat 16 Nov 2026: "standardize how
// Henry talks to himself ... the text appears just above the health bar, and I'd like to
// have all the places our mod triggers Henry to talk to himself to look the same"). The
// game's own is a bark on the player: at the map's edge, "Oof... I should go back." is
// open_world/stavy_hrace_unavahlad/.../jindrich_narazil_na_level_barieru.xml, an `ingame
// monolog` with a line of Henry's, and a quest's own (zakopanyZitrek's bark_po_smrti_mikuse)
// is the same thing asked for by the graph, RequestMonologue on the alias `player` with
// the decision's alias; the engine writes a player's bark as the plain subtitle, in white,
// centred over the health bar, with no name before it. Until now the mod wrote these
// lines itself on the HUD's narrative line ("Henry: ..." low on the right).
//
// So each line Henry says to himself is a bark of its own, made the hail's way: a
// one-line dialogue q_<id>_self_<name>.xml under the role HENRY, a token class of its own
// (the pray's shape under 4a, the letter J -- the game's files call him Jindrich -- with
// the line's slot in the byte after it), and in the graph the trigger on that token and
// the request it fires. The Lua's SelfTalk voices the words and makes the token; nothing
// of the mod's is drawn. The trigger is live always (questState.None or not): when a line
// is due is the Lua's to say, whatever the errand's state in the save.
struct SelfLine {
    int slot;            // fixed per kind of line, so a class means the same in every build
    const char* field;   // the Lua table's field that holds the words (q.enters, q.wake...)
    const char* name;    // its name in file names and string keys (self_<name>)
    std::string text;
};
std::vector<SelfLine> self_lines(const Quest& quest);
std::string self_class(const Quest& quest, int slot);
std::string self_name(const Quest& quest, const SelfLine& line);
std::string self_node(const std::string& quest_id, const SelfLine& line);
std::string self_alias(const std::string& modid, const std::string& quest_id, const SelfLine& line);
std::string emit_self_dialogue(const std::string& modid, const Quest& quest, const SelfLine& line);
// How long the engine holds a line of his it has no clip of (the Response's ReferenceLength):
// the app's speaking rate over the words (kCaptionSpeakRate), and never less than
// kSelfMinSeconds. The Lua reckons the same (SelfTalk), to know when he has done.
constexpr double kSelfMinSeconds = 1.5;
// How long past that the Lua still counts him as speaking (SelfTalking): the token's road
// to the graph and the bark's own start and end (measured 0.1 to 0.2 s over the length).
constexpr double kSelfSlackSeconds = 0.5;
double self_seconds(const std::string& text);
// How long the engine holds a line of a voiced Talk of the game's own (a holder's, a
// counsel's, a helper's, a receiver's: voiced_sequences): the Response's ReferenceLength,
// which the lines carried none of until 20 Nov 2026, so the engine reckoned each from its
// text by a rule of its own, slower than the voice -- Brother Thomas's 300-character parts
// were spoken in 21 s and the menu came back some seconds after (bugs.txt 20 Nov 2026). The
// app's voice speaks kTalkSpeakRate characters a second (measured 14.0 on four of his
// lines and four of Thomas's), and the line's audio begins kTalkLeadSeconds after the
// marker (the voice is made as it is asked for); the hold is the two together, never
// under kTalkMinSeconds. A hold a little over the voice leaves the menu a moment behind
// the last word; one under it would draw the menu over a line still being spoken, and a
// row picked then flushes the mouth queue ("next row").
constexpr double kTalkSpeakRate = 14.0;
constexpr double kTalkLeadSeconds = 1.0;
constexpr double kTalkMinSeconds = 1.5;
double talk_seconds(const std::string& text);

// The other two lines the mod says outside a conversation, as head bubbles (chat 16 Nov
// 2026: "Please do switch those to head bubbles"): a holder's `followed` row said in the
// street when his forced dialogue never opened (FollowSaid), and a beggar's thanks for a
// garment (AlmsDress). Both were on the HUD's narrative line ("<name>: ..." low on the
// right) and are the hail's bark now: a one-line `ingame monolog` under the dice-game
// role, with the hail's bubble distance and no ForceSubtitles, asked for by a token.
//
// The holder's is the hail's shape on the quest graph's `target`: q_<id>_street.xml, a
// token class of its own (the pray's shape under 42, the letter B, for bubble), and in
// the graph the trigger on it, live always as Henry's own lines' is, and the request.
bool has_street_line(const Quest& quest);
std::string street_class(const Quest& quest);
std::string street_name(const Quest& quest);
std::string street_node(const std::string& quest_id);
std::string street_alias(const std::string& modid, const std::string& quest_id);
std::string emit_street_dialogue(const std::string& modid, const Quest& quest);

// A beggar has no graph of his own to name him, so his Level's does: every beggar who
// lives on a level (Persona::level; the event beggars, who are nobody's level, in every
// one) is a SoulAsset of that Level with a token class of his own (the pray's shape under
// 54, the letter T, the tail the last twelve digits of his soul_id), a trigger on it in
// Henry's pocket and the request for the bark, the hail's nodes once per beggar. The
// dialogues are two per Level, a man's and a woman's (the role is by gender); the
// decision's alias carries the Level's name, since each Level stands its own instance.
// The words are kAlmsClothesThanks under one string key, and the tokens' inventory rows
// share one name and one line of info.
//
// Naming him by the mod's own dialogue instead -- its node's output for the role it is
// spoken through, `talk.MOONSHINE_SPEAKER`, which the shipped skill teachers' graph reads
// the teacher off -- needed no list of beggars and did nothing: the garment is given
// after his conversation has closed, and the output names nobody by then (the token made,
// no thanks said, three times over; the same request with him named by an asset spoke at
// once. README, "The last two lines off the narrative line", 16 Nov 2026).
std::string thanks_class(const Persona& beggar);
std::string thanks_name(const Persona& beggar);
std::string thanks_asset(const Persona& beggar);
bool thanks_in_level(const Persona& beggar, const std::string& level);
std::string thanks_node(bool female);
std::string thanks_alias(const std::string& modid, const std::string& level, bool female);
std::string thanks_key(const std::string& modid);
std::string emit_thanks_dialogue(const std::string& modid, const std::string& level, bool female);

// A street night's marker (has_follow; bugs.txt 28 Oct 2026: "Ernest's map quest marker
// only active when he's walking home to his love nest and for the night he's in bed, and
// disable it if he discovers he's being followed"). The objective's marker is the one on
// its current log state and nothing else can move it, so the fetch objective of such a
// quest gains two marker-only Updated states, Shown (Marker="target") and Hidden (none; the
// shipped graphs write 21 such -- klaster's GoOutside, strasidlo's Updated_DrinkingAvailable
// -- with no journal text), and two tokens of the hail's shape flip between them from the
// Lua (WatchMarker): shown in his hours, from his homecoming (routine_home_at, else the
// street watch's start) to the night window's end, and not while his night is `followed`;
// hidden otherwise. Its Started and Followed states carry no marker of their own.
std::string mark_shown_class(const Quest& quest);
std::string mark_shown_name(const Quest& quest);
std::string mark_hidden_class(const Quest& quest);
std::string mark_hidden_name(const Quest& quest);

// A street night's look hint (has_follow; chat 15 Nov 2026: "the same hint is shown when the
// priest comes into view, and remains as long as we're actively following him"). The game's
// hold-to-look prompt, as the hail has it (kHailLookSeconds below), on the holder: "Look at
// <holder>" at the lower right, Left Alt or LB held keeping Henry's view on him as he
// walks. The Lua says when (WatchLook) by two more tokens of the marker's shape: on when
// the holder on his walk home is first before Henry's eyes within kFollowLookMetres (InView,
// the warning's test), and off when the following is over -- he has turned on Henry, gone
// in, the hours are out, the errand has moved on, or Henry has let him get
// kFollowLookLostMetres ahead. Unlike the hail's, it stays up after Henry has looked.
std::string look_on_class(const Quest& quest);
std::string look_on_name(const Quest& quest);
std::string look_off_class(const Quest& quest);
std::string look_off_name(const Quest& quest);

// The giver's marker gate (bugs.txt 7 Nov 2026): while the errand is still to be taken,
// the QuestGiver marker on him is off the map when he stands where Henry would be
// trespassing to reach him, by the game's own rule (AI/crime/determineTrespassReaction.xml:
// a `private` area, no `publicServiceTrespassOverride` or `antitrespass` over it, and not
// a `semipublic`-only one by day), and back when he comes out. The Lua's WatchGiver reads
// the labels under him every tick and, on a change held for two ticks, makes one token of
// the closed or open class for the head's giverClosed / giverOpen triggers, whose
// giverHidden state the marker's IsActive is ANDed against. Two tokens per run.
std::string giver_closed_class(const Quest& quest);
std::string giver_closed_name(const Quest& quest);
std::string giver_open_class(const Quest& quest);
std::string giver_open_name(const Quest& quest);
// A scene's link told open by the mod (bugs.txt 19 Nov 2026: no marker on Father Dominic
// once Holy Jest was done): one token of this class, made by WatchGiver when the store has
// the favour before done the way the link follows, for the head's linkOpened trigger,
// Or'd with the Level's before_open port on the marker's gate. One per link.
std::string link_open_class(const Quest& quest);
std::string link_open_name(const Quest& quest);

// Libs/Tables/item/item__<modid>.xml -- the mod's own item classes, for the things a
// favour is about that the shipped tables have no carriable row for (a saw), and the
// tokens the Lua tells the graph things by, a beggar's thanks among them (thanks_class:
// one per beggar among `personas`, no quest's). Empty when there is nothing to write. The
// rows the inventory shows for them go in the same localization table as everything else.
std::string emit_item_table(const std::string& modid, const std::vector<Quest>& quests,
                            const std::vector<Persona>& personas,
                            const std::vector<LocateSoul>& locate_souls = {});
std::vector<std::pair<std::string, std::string>> item_localization(const std::string& modid,
                                                                   const std::vector<Quest>& quests);

// Node and file name of a quest inside the project: q_<quest_id>.
std::string quest_node(const std::string& quest_id);

// Name of the dialogue's Out port that a beat fires: q_<quest_id>_<command>.
std::string quest_port(const QuestBeat& beat);

// The dialogue global naming which quest beat, if any, the next loop should play.
std::string quest_variable(const std::string& modid);

// A fingerprint of which quests this build knows: FNV-1a (64-bit, hex) over the sorted
// quest ids joined by newlines. The Lua carries it as Moonshine.questSet and puts it on
// every begin and look line (`qset=`), and the app computes the same over its database
// (PersonaDb::quest_set) so a mod and an app built from different deals are caught the
// moment they meet, rather than when an accepted errand never reaches the journal
// (notes.md, 5 Oct 2026: Anna's cat, Berchta, Christine's letter). The two functions must
// agree byte for byte.
std::string quest_set_fingerprint(const std::vector<std::string>& quest_ids);

// Localization key for one of a quest's journal strings.
std::string quest_key(const std::string& modid, const std::string& quest_id,
                      const std::string& field);

// Data/Quests/<modid>.xml -- the concept graph the engine loads alongside the
// game's own (IncludeModGraphs: "Quests/%s.xml" for a mod), a Project whose
// definitions are the Levels below, one node an instance of each, the shape of the
// shipped Barbora project (a Project, its Levels under it, each level's quests under
// its own). Definition paths are resolved against the project's own directory, so it
// names <modid>/<level>.xml from here.
std::string emit_dialogue_project(const std::string& modid);

// The game's levels (Libs/Tables/level.xml), one Level node each. A Level's graph runs
// while its level is the one loaded and not otherwise, which is why there is one per
// level rather than one for all: with the dialogue under kutnohorsko alone, a Talk pressed
// anywhere around Trosky queued a request no graph was awake to run -- the words came
// through the caption, no session opened and no menu was drawn (a tester's log of 3 Nov
// 2026, every pump tick `indialog=false`). The shipped project keeps its three Levels
// `HibernateMode="Auto"` and its cross-level dialogues out of any Level; ours sit in every
// Level instead, so each level has its own instance of the same held loop.
constexpr const char* kLevels[] = {"kutnohorsko", "trosecko", "klaster"};

// Data/Quests/<modid>/<level>.xml -- one game level's Level node: the dialogue and that
// level's quests, named for the game's own level, which is what the journal files a
// quest's region by: under a bare Project every errand read "Different region" (noted
// 4 Oct 2026). It points at the dialogue definition beside it and, for each quest whose
// giver lives on this level (Quest::level), at a quest per file, with the edges from the
// dialogue's ports into them; quests of other levels are left out. Definition paths are
// resolved against this file's directory, so they name <level>/<file>.xml from here.
// The Level also stands the two dialogues of a beggar's thanks and, for each beggar among
// `personas` who lives on it, the nodes that ask for them (thanks_class).
std::string emit_quest_level(const std::string& modid,
                             const std::string& level,
                             const std::string& dialogue_name,
                             const std::vector<Persona>& personas,
                             const std::vector<Quest>& quests,
                             const std::vector<LocateSoul>& locate_souls = {},
                             bool with_guards = false);

// Data/Quests/<modid>/q_<id>.xml -- one Micro quest in the shipped shape: a
// QuestProgress state driven by two In ports, one objective whose journal logs are
// the generated text, the reward the generator priced, and a fail when the giver
// dies. The holder is given the item as a default inventory entry so it is there to
// be found; the graph never moves it, the Lua does, so the quest is the record and
// the reward and nothing else.
std::string emit_quest(const std::string& modid, const Quest& quest);

// A run: a story's scenes that share a giver, linked each to the one before it by
// after_quest_id, as one quest file and one journal entry (stages.md, 8 Oct 2026). The
// head first, then its legs in link order; a scene whose predecessor has another giver,
// or none, heads a run of its own. A quest of one scene is a run of one leg, and its file
// is what it always was. Every leg keeps its id: the talk's ports, the Lua's codes and
// store, the holder's menu and the app all address the scene, and only the file they
// are gathered in changes. The run's In ports are the legs' q_<id>_<command>, its Out
// ports the legs' q_<id>_done[_truth|_<how>]; the one questState goes Active on the
// head's accept and Done on a hand-in whose ending no scene of the run follows.
struct QuestRun {
    std::vector<const Quest*> legs;
};
std::vector<QuestRun> quest_runs(const std::vector<Quest>& quests);
const Quest& run_head(const std::vector<QuestRun>& runs, const std::string& quest_id);
std::string emit_quest(const std::string& modid, const QuestRun& run);

// The journal rows a quest needs: title, description, objective and its three logs.
std::vector<std::pair<std::string, std::string>> quest_localization(const std::string& modid,
                                                                    const Quest& quest);

// The dialogue global saying the conversation is still wanted. Lua sets it before
// asking for the session and clears it to be let out, and the loop reads it back
// through gvar() at the end of every beat.
std::string talking_variable(const std::string& modid);

// Which face the holding loop should play on the next beat. Lua writes it through
// Face() / Speak(); the graph has one sequence per value and Autoselects the match.
std::string face_variable(const std::string& modid);

// Localization key for the one thing the holding loop has to have words for. It is
// the same string everywhere it appears and never changes, so it is shipped in the
// pak and costs nothing: an id is only a problem when a conversation needs a new one
// per turn, and this one is the same on the thousandth beat as on the first.
std::string dialogue_hold_key(const std::string& modid, const std::string& name);

// The reply menu. The holding loop has one sequence whose nested decision is not
// Autoselected, so the engine presents it as a real menu -- kMenuRows rows, the last
// one the way out -- and waits, with no beat boundary and so nothing clearing the list,
// until the player picks. The rows' own prompts are placeholders under these keys; the
// words the player reads are drawn over them by Lua through the HUD's SequenceList,
// which is the one road runtime words have into the game's list widget.
//
// Six since October 2026: up to four written rows, then "(Say something else)" -- the
// row that turns into a slot for a typed or spoken line (the Lua's say-mode) -- and the
// way out. A short list draws the say row straight after its last row and the way out
// after that, so the two are indices the Lua works out per draw, not fixed ones.
constexpr int kMenuRows = 6;
constexpr int kMenuOptionRows = kMenuRows - 2;
std::string menu_row_key(const std::string& modid, int row);

// A travel is seen when Henry is this near the one he was sent to look at: a dozen paces,
// close enough to see a face and what is in a hand, far enough that no word need pass.
// The graph's DistanceTrigger and the Lua's Watch both measure it.
constexpr int kTravelSeenMetres = 12;
// An exposure's holder is caught when Henry is this near him (the width of a room: from
// outside the house the wall is between them and he sees nothing) with the one he is
// caught with this near the holder (the two sides of a double bed are a pace apart), in
// the quest's night hours. The Lua's WatchExpose measures both.
constexpr double kExposeMetres = 4.0;
// With the bedside written (npcdb.hpp has_bedside), the engine is asked for the holder's
// Talk this many milliseconds after the catching: time for Henry's waking line to be heard
// and for the token to land and the seen global to rise, so the Talk opens on the bedside.
constexpr int kWakeTalkMs = 3500;
constexpr double kExposeWithMetres = 3.0;
// Henry's aside at the door (`enters`, notes.md 23 Oct 2026): said once a night when the
// holder is home with her -- within kEnterWithMetres on the flat of her bed (the quest's
// bedPos, off routine_bed_target; the house is that wide) and in an area the level labels
// `interior`, or at her door step: outdoors but within kEnterNearMetres of the bed on the
// flat, which is her wall and nowhere else (notes.md 24 Oct 2026: the aside came seconds
// after he was through the door; where the level cannot be asked for its labels, as in
// the harness, that distance is the whole test) -- from kEnterHoursBefore hours before the
// night window opens, with Henry within kEnterSeeMetres of the holder on the flat (the
// length of the street, the door in sight) and the catching not yet made. The bed is the
// anchor and not the witness because she is about her house and yard of an evening
// (notes.md 25 Oct 2026: the aside came three times in one evening, each time the holder
// 11 m from wherever she had got to, re-armed as she moved off again), so her position
// says nothing about his; a quest with no bed spot falls back to her position in three
// dimensions. Once a night: the mark clears when the hour leaves the window (the morning),
// not when either of them moves. While a holder with a door aside or a street night is
// within kEnterSeeMetres of Henry in those hours the watch ticks every kWatchQuickMs
// instead of its usual two seconds.
constexpr double kEnterWithMetres = 12.0;
constexpr double kEnterNearMetres = 5.0;
constexpr double kEnterSeeMetres = 30.0;
constexpr double kEnterHoursBefore = 1.5;
constexpr int kWatchQuickMs = 1000;
// How many ticks of the watch, after a world arrives, grant by name the roles its souls
// lost with the load (RegrantRoles): a soul across town may not be streamed in on the
// first; ten two-second ticks is twenty seconds, long before Henry can walk to anyone.
constexpr int kRegrantTicks = 10;
// How many of the givers' marker tokens (WatchGiver) one tick of the watch may make. A new
// game has most quests off, and all of their givers' markers come off together at the first
// ticks; four a tick is eighteen seconds for the whole database, and no more than a few
// items in Henry's pocket at once.
constexpr int kGiverSwitchesPerTick = 4;
// The holder turning on Henry in the street (`followed`, notes.md 23 Oct 2026, n8). The
// watch looks for Henry through the holder's eyes every tick of the walk home: from
// kFollowHoursBefore hours before the night window opens to kFollowHoursAfter after it (his
// day links end at 21:00 give or take the hour the game jitters them by, routine_home_at in
// build_kcd2_scheduler.py; the walk is half an hour and the window 22-6), the holder
// outdoors (not in an `interior` area, and not within kEnterWithMetres of the witness, where
// the aside at the door and the catching take over), the catching not yet made, and Henry
// in his sight: within kFollowSeeMetres of him, inside the kFollowConeDegrees about the way
// he faces (GetDirectionVector) or within kFollowHearMetres from any side, and nothing
// between their eyes (System.RayTraceCheck from kFollowEyeMetres up; where the engine has no
// such call, as in the harness, the sight is clear). Twelve metres and 120 degrees since
// 25 Oct 2026 ("Ernest's detection is too sensitive"): the twenty and 140 before it caught
// Henry at the far end of a street he had only just turned into. Before any of that, when
// the walking holder is within kFollowWarnMetres of Henry in those hours (near enough that
// the next tick could be the sighting) and before Henry's eyes -- his head in the camera's
// frame (System.IsPointVisible) and nothing between their eyes (the same ray the other way;
// InView) -- Henry says his `follow_warn` to himself, once a night, so the player knows to
// hang back; the man round a corner or behind Henry earns no "there he goes" (notes.md 25
// Oct 2026, 15:33, where it came before Ernest was in sight), and a holder who sees Henry
// first forfeits it. Once the holder has turned on him and the words are over, Henry's
// `follow_over` (both notes.md 25 Oct 2026). The game's own perception
// is not used: the graph's PerceptionTrigger fired (its Function nodes ran in the autopilot
// with a quest-start toast to show for it) but the ExecuteConsoleString node it was handed
// to does nothing in the release build (probes of 24 Oct 2026, mods/kcd2/README.md, "The
// street"), and no sighting ever reached the Lua in play. Then the night is `followed`: his
// sleep links flip to his own bed (alert_tag; build_kcd2_scheduler.py), his Talk opens on
// him with the line, and the graph's Timer puts him back on the witness's bed
// kFollowRestHours of the calendar later (WorldTime; GameTime is play time and never ran
// out, 26 Oct 2026) -- past his waking, so the swap back never pulls a sleeping man out of
// a bed (an 11-hour rest from a 20:00-21:30 alert ends 07:00-08:30, when he is up and at
// his day). The Lua clears its own flag once the window is past and the world clock is
// kFollowClearHours on from the night's last word (the hour alone misread the minutes after
// the carry's winding of the clock). The console command (kGraphCommand) stays registered
// for the probes.
constexpr const char* kGraphCommand = "mn_graph";
constexpr double kFollowHoursBefore = 3.0;
// How long before the holder's homecoming hour his night marker goes on him (WatchMarker,
// markHours). The game jitters his leaving by up to the hour, and the giver's carrying
// sets Henry down at the gate half an hour before it, so a marker that waited for the hour
// itself came on a minute or two into the walk (bugs.txt 11 Nov 2026, Words Must Not Fail).
constexpr double kFollowMarkLeadHours = 1.0;
constexpr double kFollowHoursAfter = 1.5;
constexpr double kFollowSeeMetres = 12.0;
constexpr double kFollowHearMetres = 4.0;
constexpr double kFollowConeDegrees = 120.0;
constexpr double kFollowEyeMetres = 1.6;
constexpr double kFollowWarnMetres = 25.0;
// The look hint on the walking holder (look_on_class, above): up once he is before Henry's
// eyes this near -- further than the warning, since at night the hint is what finds him --
// and down again when Henry has fallen this far behind. The gap between the two keeps a
// man at the edge from flickering the prompt, and is room to hang back out of his sight
// (kFollowSeeMetres) without losing it.
constexpr double kFollowLookMetres = 40.0;
constexpr double kFollowLookLostMetres = 60.0;
// The holder set down for the carry (npcdb.hpp fast_travel_holder_to; FastTravelHolder). A
// second at a time for kCarryHolderChecks seconds after it he is looked for again, and put
// back if he is more than kCarryHolderStrayMetres from the spot: he walks a metre and a
// half a second, so further than this is the engine re-placing him for the wound clock and
// not his own feet. A holder already outdoors within kFollowLookMetres of the landing is
// left where he stands, and Henry turned to him there.
constexpr double kCarryHolderStrayMetres = 30.0;
constexpr int kCarryHolderChecks = 5;
// And the carry itself waits this long after the clock is wound before anyone is set
// down (FastTravelGo; bugs.txt 23 Nov 2026, the yard): the engine re-places every soul
// for the skipped hours a moment after the time is set, and a man set down in the same
// tick is taken off his spot by it -- Paul read 47 m from his in two seconds, and the
// forced dialogue enqueued over him played with no model in view. Set down after the
// re-placing, he stands where he is put until his own feet move him. A carry that winds
// nothing (a duel in its hours) goes at once.
constexpr int kCarryWoundSettleMs = 3000;
// A beggar's thanks for a garment, barked in his stand-in voice once the game's item
// selection has given it to him (AlmsDress): the conversation closed on the row that
// offered it, so the app is not there to say the line, and the Lua says it instead.
constexpr const char* kAlmsClothesThanks = "Oh... bless you, sir. Let me put it on, before someone takes it off me.";
// Henry's `follow_over` goes up this long after the holder's Talk closes on the `followed`
// row: he has turned away, and the subtitle of his last line is gone.
constexpr int kFollowOverMs = 2500;
constexpr int kFollowRestHours = 11;
constexpr int kFollowClearHours = 6;
// The followed token is what brings his dialogue up (the graph's forced dialogue,
// emit_followed_dialogue); the Lua looks this long after its verdict, and again
// kFollowTalkWaitMs after that, for a dialogue on the screen.
constexpr int kFollowTalkMs = 1500;
// The two stood for it (FollowFace; wl1, 16 Nov 2026, eighteen passes). The engine takes a
// forced dialogue up only if, as it is enqueued, the two are at talking distance and turned
// to each other; it does not wait for that. Facing each other at 2.7 m, the man walking up
// to Henry: nothing. Henry behind him, or ten metres before him: nothing. The man 0.7 to
// 0.9 m off and the two facing: up within half a second, every time. So the man is set
// down kFollowFaceMetres from Henry, on the line between them, turned to him, and Henry's
// view to the man, before the token is given; and the turning is kept up every
// kFollowFaceTickMs until a dialogue is on the screen, kFollowFaceMs at most, since his
// feet turn him back to his road.
constexpr double kFollowFaceMetres = 1.0;
constexpr int kFollowFaceMs = 1000;
constexpr int kFollowFaceTickMs = 50;
// The Lua used to ask for his Talk itself here (actor:RequestDialog), which only queues;
// his behaviour tree ran it or slept through it (28 Oct 2026, the user's log:
// `RequestDialog -> true` and no Talk, no line, no word to the player, and the night lost
// all the same; 16 Nov 2026, the same in play and in two passes). If no dialogue is up by
// the second look, the mod says the row itself: his `followed` line in his voice and in a
// bubble over his head (street_class), Henry's reply, then `follow_over`, so a sighting is
// never silent.
constexpr int kFollowTalkWaitMs = 4000;
// A sighting wants the holder *on his walk*: moved at least kFollowStepMetres since the
// last tick (a man standing about catches nobody behind him), and not teleported -- a step
// of kFollowJumpMetres or more between ticks is the engine re-placing him, after a clock
// jump or a stream-in, and the tick after it is for settling, not judging. And for
// kFollowGraceSeconds of real time after the giver's carrying, or after any jump of the
// world clock of kClockJumpSeconds or more between ticks (the game's own wait or sleep),
// no sighting at all: every carry in the user's logs (26-28 Oct 2026) set Henry down at
// the gate with Ernest 10-23 m off, re-placed by the clock's winding, and on 28 Oct he was
// "ahead" at 10.7 m on the first tick, before the player could move. Henry's warning to
// himself is still said in the grace, so the player knows to step back.
constexpr double kFollowStepMetres = 0.5;
constexpr int kFollowStillSeconds = 6;  // a step within this long ago is still a walk
constexpr double kFollowJumpMetres = 15.0;
constexpr int kFollowGraceSeconds = 30;
constexpr int kClockJumpSeconds = 900;
// How near Henry is before a travel target's face (seen_mood) is put on: an idle mood is
// subtle and reads at conversation distance, so it is on well before he is there.
constexpr int kTravelMoodMetres = 40;
// A giver with a hail written (npc_quest_lines `hail`) shouts it when Henry is this near
// with the errand unasked: shouting distance, far enough that he is calling Henry over
// rather than greeting him. Not again until Henry has been twice that far off and this
// many seconds have passed. The words are the bark's own bubble over his head, which the
// engine draws out to the dialogue's ChatBubblesDistance (its default,
// wh_dlg_ChatBubblesDistance, is 12 m: short of the hail), so the hail's dialogue carries
// kHailBubbleMetres, past kHailMetres with room for a step back while he speaks. The
// mod's other bubbles (a holder's row said in the street, a beggar's thanks) carry the
// same distance. Nothing of the mod's is on the HUD's narrative line any more: Henry's
// own lines left it on 16 Nov 2026 (SelfLine) and those two the same day.
constexpr int kHailMetres = 15;
constexpr int kHailBubbleMetres = 25;
constexpr int kHailAgainSeconds = 90;
// And not from a fast travel (bugs.txt 19 Nov 2026: "As fat as a prior"'s hail heard
// while the map carried Henry past its giver). The engine moves Henry along the road at
// the map's pace with the world clock wound with him, so a tick that finds him stepped
// kTravelJumpMetres or more, or the clock kTravelClockSeconds or more on, since the tick
// before is a travel (or a load, a wait at a bench, a sleep -- all of them a moment nobody
// should be shouting through), and WatchHail holds its tongue for kTravelGraceSeconds of
// real time after the last such tick. A giver by the road he arrives on hails him once he
// has stood there that long. 30 m a tick is three times a sprint's two seconds; two game
// minutes a tick is twice what the clock does on foot.
constexpr double kTravelJumpMetres = 30.0;
constexpr int kTravelClockSeconds = 120;
constexpr int kTravelGraceSeconds = 6;
// The scene after, led on to (QuestLead; bugs.txt 19 Nov 2026): how often the Lua looks
// for the game's Talk to have come down after the row that completed the scene before,
// and how long it keeps looking before the lead is given up.
constexpr int kLeadTickMs = 500;
constexpr int kLeadWaitSeconds = 30;
// A session asked for while the game's own Talk was still playing out (bugs.txt 22 Nov
// 2026: Father Adam's lead, the handover's reply still on screen) opens nothing of ours --
// IsInDialog stays up on the old talk, the twin is gone, and no beat ever comes, while the
// app's lines are captioned over it with no menu to follow. Ours beats inside three seconds
// of opening (the second response's StartDelay); this many pump ticks (0.4 s each) with none
// is taken as the dialogue up not being ours, and it is cut and ours asked for again, once.
constexpr int kNoBeatTicks = 20;
// The wave that goes with the shout (HailWave, 19 Nov 2026): the engine's urgeDialog
// behaviour waves big past five metres and small within, and the clip is started on this
// animation layer, which a probe on a standing townsman found takes an upper-body "_over"
// clip without upsetting his idle.
constexpr int kHailWaveBigMetres = 5;
constexpr int kHailWaveLayer = 1;
// How many times the wave is played through, one after another (bugs.txt 20 Nov 2026: once
// was over before Henry had looked up).
constexpr int kHailWaveTimes = 5;
// One tap of the skip key in a voiced Talk of the game's own arrives as two button actions
// (dialog_cancel and dialog_skip) in the same frame; a second within this many seconds of
// the first is the same tap (OnPlayerAction; bugs.txt 19 Nov 2026). Well under the fastest
// a player taps through two rows, which is a fifth of a second or more.
constexpr double kSkipLineDebounceSeconds = 0.1;
// The disputation (npcdb.hpp DebateStage; bugs.txt 19 Nov 2026): Henry gives chapter and
// verse on his own if his Scholarship is this or more (soul:GetSkillLevel, the game's own
// scale of 1-30), or on the counsel's word whatever his learning.
constexpr int kDebateScholarship = 25;
// The order of Henry's three answers on each stage's menu, a different one per stage so
// the sound answer is not always the top row.
constexpr const char* kDebateOrders[][3] = {
    {"plausible", "sound", "common"},
    {"common", "plausible", "sound"},
    {"sound", "common", "plausible"},
};
// With the shout goes the game's own "hold to look" hint (emit_leg, hailLook): "Look at
// <giver>" at the lower right, Left Alt or LB held turning Henry's view to him. It is up
// this many seconds, or until Henry has looked or taken the errand: long enough to find
// the key after the shout has ended, short enough not to sit in the prompt slot.
constexpr int kHailLookSeconds = 8;
// A giver's household, for a quest with `welcome` (npcdb.hpp, kHouseholdMetres), carries
// the game's `crime_trespass_expected` context from accept on, so Henry at their hearth is
// a guest, not a trespasser: the preset the shipped expectedeffect module sets, and the
// context that keeps it until he has left the private area.
constexpr const char* kWelcomePreset = "crime_trespass_expected";
constexpr const char* kWelcomeExtended = "crime_trespass_expected_extended";
// A giver whose day is given over to grief (Quest::routine) says nothing to a passing
// Henry: the game's own preset for an NPC who takes no notice of the player --
// `crime_dontGreetPlayer` (44 shipped quests set it on a soul for a while -- a cart's
// passengers, a tour's party, Ptacek `behem_questu`), `crime_dontCommentPlayersDog` and
// `..Horse` (the shipped `vypnout_nevhodne_barky`, "turn off the unsuitable barks", sets
// the first two together), and the ignores for crouching, a drawn weapon, dropped items
// and no torch (ScriptContextPreset.xml). The greeting context alone left her saying
// "What a good doggy" to Mutt over the crying (notes.md, 8 Oct 2026).
constexpr const char* kRoutineQuiet = "crime_dontCommentOnPlayer";
// The scheduler tag the graph flips for a quest's routine; scripts/build_kcd2_scheduler.py
// writes the same name into the level's scheduler override, so the two must agree.
std::string routine_tag(const std::string& modid, const std::string& quest_id);
// The tag the graph holds on while an exposure's holder has caught Henry following him
// (has_follow): the scheduler override's split of his sleep links reads it, his own bed
// under the tag and the witness's under it inverted. The same script writes the name.
std::string alert_tag(const std::string& modid, const std::string& quest_id);
// The tag the graph turns on at a holder's handover and never off (Quest::has_gone): the
// scheduler override's added link on his NPC hub, to a wrapper by the camp he goes to,
// reads it. The same script writes the name.
std::string gone_tag(const std::string& modid, const std::string& quest_id);
// The tag the graph turns on kWedHours of the calendar after that handover, for good, as
// it takes the gone tag off (Quest::has_wed): the pair's day links in town and their shared
// night link, added to both NPC hubs by the same script, read it. Twenty hours: the camp
// that afternoon and night, and the walk into town the next morning.
std::string wed_tag(const std::string& modid, const std::string& quest_id);
constexpr int kWedHours = 20;
// The tag the graph turns on as a duel's carry begins and off when the duel is done
// (Quest::has_yard): the scheduler override's added link on the man's NPC hub, to the
// wrapper his duel spot is written on, reads it, so the scheduler's pull and the Lua's
// set-down agree and the forced dialogue finds him on the spot (bugs.txt 25 Nov 2026).
std::string yard_tag(const std::string& modid, const std::string& quest_id);
// And the tag on from the cowed token to the duel's end (Quest::has_kneel): the added
// link to a kneeling wrapper by routine_cowed_target reads it.
std::string kneel_tag(const std::string& modid, const std::string& quest_id);
// The yard token is made half a second before the carry's clock is wound, so the graph
// has the tag on before the engine re-places the souls for the skipped hours.
constexpr int kDuelYardLeadMs = 500;
// A travel with a brawl stages it on that same match: the brawlers are set at odds with the
// one looked at, told to fight bare-handed and to be nobody's business (`combat_invisible`,
// so neither Henry nor a guard is drawn in), and each is sent the game's own first-punch
// message this many seconds after the match -- the relation alone starts nothing (proven
// on the Windows laptop, behaviors.md §7). The fight is stopped at the first knock-out or after this many
// seconds of game time, whichever is first, and the loser's waking reaction is held down
// for the aftermath so he does not come round shouting robbery.
constexpr int kBrawlKickSeconds = 3;
constexpr int kBrawlSeconds = 60;
constexpr int kBrawlAftermathMinutes = 3;
constexpr const char* kBrawlContexts[] = {
    "combat_indifferenceUnarmed", "combat_forceUnarmedWhileInUnarmedSkirmish",
    "combat_neverSurrenderOrFlee", "combat_neverAcceptSurrender", "combat_invisible",
};
// An ambush (kind "ambush", quest-design/set-aside-stories.md §5) is the same fight with
// Henry on the receiving end: the one he was sent to is spoken to and gives directions to
// a place (spot_pos) and an hour (hour_from/to); when the Lua's Watch has him there in the
// window with the directions in hand, its token fires the graph's trigger and the brawlers
// are set on the `player` asset. Bare-handed and ready to run when beaten, since they are
// robbers, not a duel; not invisible, since Henry is in it; stopped at the first man down on
// either side -- Henry's own knock-out ends it too -- or after this many seconds. Henry is
// at the place within this many metres of the spot: a graveyard, not a doorstep.
constexpr int kAmbushSeconds = 75;
constexpr double kAmbushMetres = 12.0;
constexpr const char* kAmbushContexts[] = {
    "combat_indifferenceUnarmed", "combat_forceUnarmedWhileInUnarmedSkirmish",
    "combat_neverAcceptSurrender",
};
// A duel (kind "duel"; bugs.txt 20 Nov 2026, In the Stocks) is an ambush with the man who
// set it in the fight himself: armed, to the death, neither side giving or taking quarter
// until the Lua's cowed token (his two made friends down) stops it and he begs; nobody's
// crime, since he began it in an empty yard at night (combat_invisible, as a travel's
// brawl has it). Long enough for three men to be fought.
constexpr int kDuelSeconds = 240;
constexpr const char* kDuelContexts[] = {
    "combat_neverSurrenderOrFlee", "combat_neverAcceptSurrender", "combat_invisible",
};
// Where the two made ones stand as Henry lands (metres behind him, a pace to either side),
// the sword put in their hands (the shipped CombatDebug.lua's shortsword weapon preset),
// and how far off Henry must be for their bodies to be taken away (DuelSweep).
constexpr double kDuelBehindMetres = 3.0;
constexpr const char* kDuelSwordPreset = "b966514a-966a-4b1c-b870-755abd25bd32";
constexpr double kDuelSweepMetres = 80.0;
// A duel with a taunt (npcdb.hpp duel_taunt; bugs.txt 22 Nov 2026) makes the men and
// begins the brawl at the taunt dialogue's end (DuelTaunted), or this long after the
// landing when the dialogue has not played (WatchDuel): a forced dialogue the game will
// not open leaves the yard quiet otherwise; a dialogue that is up is never cut (WatchDuel
// reads InDialog). The 23 Nov run's say ran twenty seconds and the watch loosed the men a
// breath before its end, so the count is from the token and longer than two lines.
constexpr int kDuelTauntSeconds = 40;
// The say is asked for this long after the landing, not in the same tick (bugs.txt 23 Nov
// 2026: the dialogue enqueued as Henry and the man were set down played with no model in
// view, and the man read 28 m off when it ended): the engine has them both placed and the
// camera somewhere to be; the man is held on his spot through it (DuelSpring, DuelSay).
constexpr int kDuelSayDelayMs = 2500;
// How far a duel's man may stand from his spot before the watch puts him back on it,
// while his say is waited on (DuelHold; bugs.txt 23 Nov 2026: at 28 m he was nowhere to
// be seen): a few paces, not the carry's thirty metres, since nothing of his is near.
constexpr double kDuelHoldMetres = 8.0;
// Henry's look, for a holder whose first excuse is cut to it (npcdb.hpp has_look; bugs.txt
// 22 Nov 2026). Armour is the quest graph's to read -- I_Soul::CheckEquipment over the five
// slots the game's own players_appearance_scan.xml asks after (body_chainmail, body_plate,
// head_helmet, leg_armor, sleeves), fed to the holder's dialogue as its `bare` port; the Lua
// cannot read what is worn. Dress is the Lua's: a fop when his relative charisma (rch, 0..1;
// 0.31 stripped, 1.0 in fine clothes on the measured save) is at or above this. The game's
// danger look (bad) was tried and dropped: it read 1.0 in fine clothes.
constexpr double kLookFopCharisma = 0.7;
// How long after the men are made WatchDuel waits before one it cannot find, or finds
// down, counts as beaten: a fresh spawn answers IsUnconscious before it stands (the 22 Nov
// run cowed the man eight seconds in with both his friends on their feet), and a man
// never seen standing is not a man beaten.
constexpr int kDuelStandSeconds = 15;
// The yard's walls (bugs.txt 24 Nov 2026): the top of the man's head showed over something
// as Henry landed, the forced dialogue's camera looked at a wall, and one made man never
// came -- the written spot and a spawn point can be the far side of a wall from the
// landing. Before anyone is set down a ray is cast at kDuelEyeMetres between the landing
// and the point, and a point with a static hit on the way is passed over for the next:
// the holder's spot is tried as written, then the same distance from the landing on the
// other three quarters; a man's point behind Henry is tried where written, then nearer,
// then at Henry's flanks. A made man still standing kDuelStuckSeconds after the loosing,
// further than kDuelStuckMetres from Henry and not two metres from where he was made, is
// taken for stuck and put down again on a clear point behind Henry, once.
constexpr double kDuelEyeMetres = 1.5;
// And once the men are loosed the man is kept within kDuelLeashMetres of Henry through the
// fight and after he is cowed, until the duel is done (bugs.txt 24 Nov 2026, "Paul didn't
// seem to be present during or after the fight"): a soul out of Henry's sight with his
// evening's seat to go to is warped along his way by the scheduler -- 42 m in a second,
// read toward the tavern, twice in one landing -- and once the say no longer holds him he
// goes. Further than the leash he is put down kDuelLeashBackMetres from Henry on the side
// his spot is, facing him; nearer he is his own, so his fighting is not fought with.
constexpr double kDuelLeashMetres = 14.0;
constexpr double kDuelLeashBackMetres = 4.0;
constexpr int kDuelStuckSeconds = 12;
constexpr double kDuelStuckMetres = 6.0;
// A pray is prayed when the player soul's praying context -- the game's own "Player
// praying" script context, set while Henry kneels at a shrine and cleared when the prayer
// ends (measured on the Windows laptop: false, true 8.6 s after the shrine's Pray prompt is used, false
// again 53 s on, the Guardian Angel buff landing at that moment) -- is seen up with Henry
// within this of the spot (metres), and then seen down again after at least this many
// seconds: the game lets a prayer be broken off (the Stop prompt), and a kneel that rose
// in a few seconds is not one said; the shortest of the game's prayers runs near forty.
// The Lua's Watch, on its two-second tick, reads both; the graph sees the token.
constexpr double kPrayMetres = 2.5;
constexpr int kPrayKneelSeconds = 20;
constexpr const char* kPrayContext = "player_inPrayingUnstance";
// The perk that lets the player pray at shrines: Basic Theology (Scholarship 14, perk__kcd2.xml
// "Zaklady bohoslovectvi meta", 70e5d9ba-3b26-4196-864f-755c1b51ea7d) is a meta perk the
// soul never reports holding; what it grants, and what HasPerk answers for, is this child.
// A pray is not offered to a player without it (measured on the Windows laptop: HasPerk(guid, false) is
// the call, AddPerk/RemovePerk of the meta grant and strip the child).
constexpr const char* kPrayPerk = "d23569a9-0f9a-4328-8eb5-2bbc0d0ccdd4";
// A mind's visits are due this many hours of game time after the last (Calendar world
// time); the first is due at once. The Lua's Watch keeps the dialogue global the target's
// menu reads.
constexpr int kMindGapHours = 6;
// A leave or a return (quest-design/set-aside-stories.md §2, §4): Henry is at the place
// within kSpotMetres of spot_pos; there, in the hour window, with the thing on him, the
// leave key (F9, the typing toggle's own map, armed only while he stands there) puts it
// down (human:PlaceItem on a spawned spot entity), and a drop by hand -- the count gone to
// nothing while he stands there -- counts the same. The cue (cue_sound) plays from a spawned
// AudioTriggerSpot kCueMetres off, kCueDelaySeconds after the thing is down, and is heard
// if Henry is still within kCueHeardMetres of the place. A return's grave is robbed when
// any loot_classes count on Henry rises within kLootMetres of the spot; its curse is laid
// when he is kCurseMetres from the spot with the goods still on him or the grave robbed,
// as the game's plague buff for kCurseHours of game time, lifted early if he comes back
// and puts them down. The coughs come every kCoughSeconds or so while he is within
// kCoughMetres of the spot.
constexpr double kSpotMetres = 4.0;
constexpr double kCueMetres = 20.0;
constexpr int kCueDelaySeconds = 25;
constexpr double kCueHeardMetres = 40.0;
constexpr double kLootMetres = 10.0;
constexpr double kCurseMetres = 40.0;
constexpr int kCurseHours = 48;
constexpr double kCoughMetres = 8.0;
constexpr int kCoughSeconds = 18;
// A seek (npcdb.hpp path, Sweepings and Strays): the giver's animal is the mod's own, a
// BasicEntity on the game's cat character (kSeekCat) spawned at the first point of the
// path once Henry is within kSeekCallMetres of it, sitting and calling (cue_sound, every
// kSeekCallSeconds while he is within hearing). When he is within kSeekNearMetres she goes
// on to the next point at kSeekSpeed metres a second (galloping; the position is moved
// each stride) and sits and calls again; at the last point (spot_pos, raised by the
// quest's spot_lift where the floor's look is above its physics, a straw heap) she lies
// down and the litter is spawned at her right side (the kSeekKittens definitions, at
// kSeekKittenScale, in a row kSeekKittenGap apart along her, kSeekKittenSpread out from
// her, each nudged by its kSeekKittenWobble so the row is a litter's and not a rank's;
// bugs.txt 9 Nov 2026). Henry within kSeekFoundMetres of the litter is the find: the
// store's seen, the graph's token (log_found). The family stands until the hand-in,
// spawned again if a load swept it and he comes back.
constexpr double kSeekCallMetres = 40.0;
constexpr double kSeekNearMetres = 5.0;
constexpr double kSeekFoundMetres = 4.0;
constexpr int kSeekCallSeconds = 6;
constexpr double kSeekSpeed = 2.5;
// How often her position is moved while she runs (SeekStride): a timer chain of its own
// at 20 Hz, since the watch's quick tick is a second and a cat set down 2.5 m at a time
// in the gallop clip read as teleporting (bugs.txt 9 Nov 2026).
constexpr int kSeekStrideMs = 50;
// When the family leaves the spot after the hand-in (bugs.txt 9 Nov 2026, "Litter should
// vanish the day after the quest is handed in"): the first watch tick at or after this
// hour of the next game day with Henry out of hearing of the spot, the giver having
// fetched them home. Until then they lie there, and are made again after a load.
constexpr int kSeekLeaveHour = 6;
// Henry's asides (bugs.txt 9 Nov 2026): `spotted`, said once when the cat is first before
// his eyes -- sitting at a point within kSeekSpotMetres, in the frame and nothing between
// (InView, with the cat's own height) -- and `discovered` at the find. Neither over a
// conversation; the first kept in the store so a load does not have him say it twice.
constexpr double kSeekSpotMetres = 25.0;
// How far from Henry her call is placed when she is farther than this (SeekVoice): the
// game's cat cues carry about six metres (the probes of 9 Nov 2026: heard at 3 and 6, faint
// at 9, nothing from 12), so the sound spot is set on the line towards her at this
// distance, and at her once she is nearer. A fresh spot per call, since a spot plays a
// trigger once.
constexpr double kSeekVoiceMetres = 4.0;
// How long she sits at a point before Henry's closeness can move her on (bugs.txt 10 Nov
// 2026, "I hear Maggie at the first point in the path, but not on subsequent stops"): on
// short legs he was within kSeekNearMetres as she arrived and she bolted on the next tick
// without a call. Now she calls the tick she arrives, and goes on only once she has
// called at that point and sat this long.
constexpr int kSeekDwellSeconds = 2;
constexpr double kSeekEyeMetres = 0.3;
constexpr double kSeekKittenScale = 0.3;
constexpr double kSeekKittenSpread = 0.35;
constexpr double kSeekKittenGap = 0.15;
// Each kitten's nudge off its place in the row: along her (m), out from her (m), and the
// turn of its facing off square-on to her (degrees). Fixed, not rolled, so the litter
// lies the same after a load as before it.
struct KittenWobble { double along, out, turn; };
constexpr KittenWobble kSeekKittenWobble[] = {
    {0.02, -0.03, -18}, {-0.03, 0.04, 25}, {0.01, 0.02, -8}, {0.03, -0.04, 15}, {-0.02, 0.01, -28},
};
constexpr const char* kSeekCat = "Objects/characters/animals/cat/cat.cdf";
// The litter: the four coats, and a second grey tabby, the mother's own.
constexpr const char* kSeekKittens[] = {
    "Objects/characters/animals/cat/moonshine_kitten_tabby.cdf",
    "Objects/characters/animals/cat/moonshine_kitten_ginger.cdf",
    "Objects/characters/animals/cat/moonshine_kitten_white.cdf",
    "Objects/characters/animals/cat/moonshine_kitten_black.cdf",
    "Objects/characters/animals/cat/moonshine_kitten_tabby.cdf",
};
static_assert(sizeof kSeekKittenWobble / sizeof kSeekKittenWobble[0] == sizeof kSeekKittens / sizeof kSeekKittens[0],
              "a wobble for each kitten");
// A find's keepsake (npcdb.hpp keepsake_class): laid at accept, and if that failed (the
// spot too far out to place at, a load that wiped it) laid again from the watch once Henry
// is within kKeepsakeMetres of the spot without it.
constexpr double kKeepsakeMetres = 120.0;
// How far above the ground the keepsake is set down (bugs.txt 18 Nov 2026: the carved heart
// lay flat in the marigolds and was hard to see). PlaceItem puts the item at the spot
// entity; the spot is raised this much off the ground hit, so the thing sits proud of the
// flowers rather than in them. A small figure on purpose: a placed item is a sleeping rigid
// body, and if the engine wakes it, it has only this far to fall.
constexpr double kKeepsakeLiftMetres = 0.35;
// And how much bigger than the item table's model it is drawn (bugs.txt 19 Nov 2026:
// "double the size of the object"). The item table has no scale of its own, so the placed
// item's entity is found in the world after PlaceItem (within kKeepsakeScaleMetres of the
// spot, the one entity there that is neither the spot nor a soul) and scaled
// (entity:SetScale), tried again from the watch until it is, and said in the log.
constexpr double kKeepsakeScale = 2.0;
constexpr double kKeepsakeScaleMetres = 1.5;
// And Henry's word that he has come to the patch (Quest::arrived), once he is within
// this of the spot: close enough to see the flowers, far enough that it is said as he
// walks up rather than as he stoops.
constexpr double kFindArriveMetres = 15.0;
// A retrieve's chest (npcdb.hpp chest_pos): spawned from the watch once Henry is within
// kChestMetres of the spot with the errand on -- the width of a town street or two, so it
// stands before he can be at the door, and nobody's chest is made in a town he has not
// come to.
constexpr double kChestMetres = 80.0;
// The game's caption holds about four lines of our words before they run off the plate
// (notes.md 14 Oct 2026); a line longer than kCaptionPageChars is shown in pages, turned
// as the voice should reach them at kCaptionSpeakRate characters a second (a steady
// speaking pace, a little under 180 words a minute).
constexpr int kCaptionPageChars = 240;
constexpr int kCaptionSpeakRate = 15;
// A voiced Talk of the game's own has no pump of ours running; its paged caption turns
// on a timer of its own at this period (the pump's own, 400 ms; bugs.txt 19 Nov 2026).
constexpr int kTalkPageTickMs = 400;
constexpr const char* kCurseBuff = "c3ceb785-f7b8-4176-ad5a-7f9c5f0353aa";  // quest_nakaza_plague
// The mod's own Smelly debuff (assets/pak/Libs/Tables/rpg/buff__moonshine_names.xml): a day of
// lowered charisma for the bucket of guts a holder answers a refused bribe with.
constexpr const char* kSmellyBuff = "a1c3be55-84fc-5eba-a1a2-2a57e539463f";
// What the bucket sounds like, played on Henry (data/kcd2/sounds/atl_triggers.tsv): the
// game's wet laundry hitting a board, then its general liquid plop.
constexpr const char* kGutsSlapSound = "f_vi_laundry_wet_fabric_hit";
constexpr const char* kGutsSlopSound = "f_ge_liquid_plop";
constexpr const char* kCoughSound = "f_ge_cough_man";
constexpr const char* kWindSound = "q_M12_wind_tunnel";
// The floor under a point (the Lua's Ground): a ray dropped from kGroundAboveMetres above
// the height given, meeting the built world or the terrain, taken when it lands within
// kGroundBandMetres of that height. The terrain's own height is under a building's floor
// -- a yard's straw, a shed's boards -- and a thing set at it there is sunk out of sight
// (bugs.txt 9 Nov 2026, the cat and kittens unseen at the hen yard). The start is a cat's
// height up, under any lintel; the band is a step or two.
constexpr double kGroundAboveMetres = 1.2;
constexpr double kGroundBandMetres = 2.0;
constexpr const char* kBatsSound = "a_o_netopyr_scared";
constexpr const char* kBatsEffect = "WH_Particels.other.bats";
constexpr const char* kDustEffect = "cinematics.other.dust_hit";
// The player's soul, as every shipped graph names it (Haste.xml, Barbora.xml).
constexpr const char* kPlayerSoulGuid = "4c2dcffb-dea1-6263-72d7-b39f4db2d8b5";

// Data/Quests/<modid>/<name>.xml -- one player-initiated dialogue that says nothing
// at all. Every soul holding role can speak it, since the role is what binds a line
// to a soul.
//
// It carries no words because words are what used to make it finite. A line reaches
// the screen through a localization id, an id keeps the first wording it is given
// for the rest of the run of the game, and an id only gets there by being named in
// a sequence authored ahead of time -- so a conversation of any length wanted a slot
// per turn and ran out. Game.SendInfoText takes the text itself and needs none of
// that, which leaves this with one job: holding the focused camera on the speaker
// and the HUD out of the player's way while the words arrive by another road.
//
// So it is a loop rather than a script, in the shape of the shipped
// Quests/Testing/.../hracovo_modleni.xml: a beat of two wordless responses, then a
// pair of conditions on gvar('<modid>_talking') that either send the graph back to
// the top or end the session. beat_seconds is how long one turn takes, waited out as a
// StartDelay on the second response -- which is what stops a wordless beat finishing in
// the frame it starts, ReferenceLength being ignored when there is no line to time.
//
// A second global, gvar('<modid>_face'), picks among those beats: idle (camera only),
// speak (OpenMouth/Hmm for a Lua-timed stretch), the shipped FacialMood names, and
// the one-shot ADLG_FA_ head clips. Commands are static, so each value is its own
// sequence; Autoselect plus exclusive EntryConditions is the same round trip as the
// hold/leave branch.
//
// A third global, gvar('<modid>_quest'), adds one sequence per QuestBeat: the same
// wordless beat, but carrying <Triggers> that fire the dialogue's Out port for that
// quest command. The face sequences all require it to be 0, so while a beat is asked
// for exactly one sequence is available, and its ExitScript hands the code back.
std::string emit_dialogue(const std::string& modid,
                          const std::string& name,
                          const std::string& role,
                          double beat_seconds,
                          const std::vector<QuestBeat>& beats);

// Data/Quests/<modid>/<level>/guard_talk.xml -- adds "Can you help me find someone?"
// as a Priority="General" root option on friendly guards without overriding their
// existing conversation options (such as "I want to turn in a gang"), then transitions
// into the held Moonshine loop under Role="MOONSHINE_GUARD".
std::string emit_guard_dialogue(const std::string& modid,
                                const std::string& name,
                                const std::string& role,
                                double beat_seconds);

// Data/libs/Storm/names/<modid>.xml -- one hasName/setUiName rule per soul.
// A non-zero rule_order writes an explicit <rule order="..."> so our rules beat
// the base game's social-class rules regardless of source ordering.
std::string emit_storm_rules(const std::string& modid,
                             const std::vector<Persona>& personas,
                             int rule_order);

// Localization/English_xml.pak/text__<modid>.xml -- the stringId table. Matches
// the shipped files byte for byte in shape: UTF-8 with no BOM, no XML
// declaration, CRLF line endings, three cells with the second one ignored.
// extras are written after the persona names as further stringId/text rows.
std::string emit_localization(
    const std::string& modid,
    const std::vector<Persona>& personas,
    const std::vector<std::pair<std::string, std::string>>& extras);

// How a line of conversation gets onto the screen.
//
// Three roads lead there and they are not alike. UIAction writes into the caption the
// game says its own dialogue in, which looks like the game because it is the game, at
// the price of the dialogue writing there too. Game.SendInfoText hands the words to the
// HUD widget the game uses for its own notices, which then owns them: it decides where
// they sit, what they look like, and it queues them, so a line can wait behind something
// the game wanted to say. System.DrawText puts them where it is told in whatever size it
// is given, and owns nothing -- it draws for the one frame it is called on, so keeping a
// line up means asking again every frame.
struct ScreenText {
    // "caption" for the dialogue caption through UIAction, "draw" for System.DrawText,
    // "info" for Game.SendInfoText.
    //
    // The caption is the one to want and it took the longest to find. It is the widget the
    // game says its own dialogue in, so the words arrive in the Scaleform face no font
    // definition can reach, on the background no drawing primitive could be made to
    // render, in the place a player already looks, wrapped. And it takes a raw string:
    // SetSubtitles(Text) keeps nothing, so it costs no stringId, which is the whole reason
    // a conversation can run as long as it likes.
    //
    // What it costs is that the widget is not ours. The dialogue writes its own line there
    // as each response begins, so every line has to be put back rather than set -- see
    // captionEvery in the generated Lua.
    std::string mode = "caption";
    // Where the drawn text goes, as a fraction of the viewport: x of the middle of the
    // line and y of its top. Not pixels, which is what DrawText itself wants -- 400, 500
    // was measured landing a tenth across and a third down a 3840x1600 screen, so the
    // same numbers would sit somewhere else entirely on another monitor.
    // System.GetViewport reports the real size, so the fractions are turned into pixels
    // in the game.
    double x = 0.5;
    double y = 0.82;
    // Size, the font definition the mod replaces, and the face to put in it.
    //
    // "default" because that is the one the engine draws with and DrawText offers no way
    // to pick another: it loads %engine%/Fonts/default.xml for itself, and the mod ships
    // a fonts/default.xml of its own to sit over it. AlexanderQuill is the only period
    // face in Engine.pak/Fonts, used there by subtitles.xml; the other three definitions
    // all name VeraMono, which is the monospace that debug text comes out in and what
    // these words looked like before.
    // 3 because it was picked out of a ladder of six sizes drawn on screen as the one
    // nearest the game's own subtitles, and because it is what the shipped debug script
    // passes. The unit is neither pixels nor anything else nameable -- there is a cvar
    // called r_AuxGeomFontAutoScaleMultiplierOver1080pWH, so it is scaled by the display
    // before it reaches the glyphs.
    double size = 3;
    std::string font = "default";
    std::string face = "AlexanderQuill.ttf";
    // Pixels a character takes per unit of size, which is only ever a guess: nothing in
    // the bindings measures a string, so the width of a line has to be estimated to centre
    // it. Being wrong moves the line off centre and costs nothing else.
    //
    // 9 works back from what was seen: a 38-character line drawn as if a character took 16
    // sat a little left of centre, which puts the real figure nearer 13, and 13 over a
    // size of 3 with the half below is what this is.
    double pixels = 9;
    // How often the drawn line is put back, in ms. Below one frame on purpose: text
    // that misses a frame flickers, and there is no cost to asking twice in one.
    int repaint_millis = 10;
};

// Fonts/<name>.xml -- a font definition of the engine's own kind, shipped to sit over
// one of the four in Engine.pak/Fonts.
//
// Replacing rather than adding, because DrawText was measured to have no font parameter
// at all: the reference lists one, the engine warns on every call that passes one, and
// the game's own debug script passes a number in that place. So the face these words
// come out in is not something a caller can choose -- the only lever is what the font
// the engine already draws with is defined as, and LoadFont(name) reads fonts/<name>.xml,
// which is a path a mod pak can sit on like any other.
//
// The shape is the shipped one: a face, then effects made of passes. The second pass is
// the same black offset by a pixel, which is the shadow shipped subtitles.xml puts behind
// its glyphs and the reason a caption stays readable over a bright field.
std::string emit_font_xml(const std::string& face);

// Data/Scripts/mods/<modid>.lua -- the engine's per-mod entry point, which it
// loads by modid on startup, so nothing shipped has to be overridden. Carries
// the soul allowlist and adds one interactor option to the NPC menu. role is the
// one our dialogue speaks through and metarole is the group holding it, which is
// what a running game can still hand to a soul STORM already finished with.
// leave_hint is shown on its own as a conversation starts, since a focused
// conversation offers no other sign of how to get out of it. inboxes are the
// paths a turn's words are looked for under, tried in order. quests are what the
// "@quest" directives at the front of a turn can name: the Lua moves the item on
// handover, takes it back on complete, and asks the graph for the beat that fires
// the quest's port for accept and complete.
std::string emit_conversation_lua(const std::string& modid,
                                  const std::vector<Persona>& personas,
                                  const std::string& role,
                                  const std::string& metarole,
                                  const std::string& leave_hint,
                                  const ScreenText& screen,
                                  const std::vector<std::string>& inboxes,
                                  const std::vector<Quest>& quests,
                                  const std::vector<LocateSoul>& locate_souls = {},
                                  const std::vector<GuardSoul>& guards = {},
                                  const std::vector<std::string>& story_souls = {});

// Data/Libs/Config/<modid>_typing.xml -- a keyboard, as two actionmaps, which the
// conversation Lua loads at runtime with ActionMapManager.LoadFromXML rather than by
// overriding defaultProfile.xml. One holds F9 alone and is on for the length of a
// conversation; the other is exclusive, binds every key a reply could need, and is on
// only while a line is being typed, so that space stops meaning skip and w stops
// meaning walk. Each key reaches Player:OnAction as its own action, mtype_<key>, which
// is the only way the engine hands input to Lua. Measured on retail: an exclusive map at
// this priority took every letter during a conversation and let nothing but the mouse
// axes past.
std::string emit_typing_actionmaps(const std::string& modid);
// The path inside the pak the Lua asks for it by.
std::string typing_actionmaps_path(const std::string& modid);

// The loose mod.manifest that sits beside the paks.
std::string emit_manifest(const std::string& modid,
                          const std::string& name,
                          const std::string& description,
                          const std::string& version,
                          const std::string& author,
                          const std::string& kcd_version,
                          const std::string& created_on);

}  // namespace kcd2
