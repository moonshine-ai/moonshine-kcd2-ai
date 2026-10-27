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
                                const std::vector<Quest>& quests);

// Data/libs/Tables/rpg/role__<modid>.xml -- declares the role before anything
// grants it or speaks through it. Unlike a setUiName key, a role is not a
// free-form identifier: <addRole> and a dialogue's Role attribute both resolve
// against this table, so one that is missing from it silently matches nothing.
// An additive fragment rather than a patch of the 745KB original, which is how
// the shipped tables split themselves 981 times over.
std::string emit_role_table(const std::string& modid,
                            const std::string& role,
                            const std::string& metarole,
                            const std::vector<Quest>& quests);

// Data/libs/Storm/roles/<modid>.xml -- grants each soul the role our dialogue
// speaks through, which has to be one emit_role_table declared, and each quest's
// holder that quest's role.
std::string emit_storm_roles(const std::string& modid,
                             const std::vector<Persona>& personas,
                             const std::vector<std::string>& only_souls,
                             const std::string& role,
                             const std::vector<Quest>& quests,
                             std::size_t* granted);

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
std::string token_name(const Quest& quest);
// A chest's second token: made when the thing has left the chest for Henry's pocket
// (WatchChest), for the trigger that completes the fetch objective (taken_q_<id>).
std::string taken_class(const Quest& quest);
std::string taken_name(const Quest& quest);
// A chest's paper: the Document laid beside the thing (paper_q_<id>), and its page's
// localisation key.
std::string paper_class(const Quest& quest);
std::string paper_name(const Quest& quest);
// A dictated letter's Documents (Quest::has_dictation): one class per combination of the
// parts' outcomes, the three digits spelt into the guid between a fixed prefix and a
// suffix from the quest id, so the Lua builds the class from the store's digits with no
// table (LetterClass). The item row's name is the letter's with the digits appended.
std::string letter_class_prefix();
std::string letter_class_suffix(const Quest& quest);
std::string letter_class(const Quest& quest, const std::string& wrote);
std::string letter_variant_name(const Quest& quest, const std::string& wrote);
std::string paper_page_key(const std::string& modid, const Quest& quest);

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
std::string helper_metarole(const std::string& quest_id);
std::string helper_role(const std::string& quest_id);
std::string helper_node(const std::string& quest_id);
std::string key_class(const Quest& quest);
std::string key_name(const Quest& quest);
std::string emit_helper_dialogue(const std::string& modid, const Quest& quest);
std::vector<std::pair<std::string, std::string>> helper_localization(const std::string& modid,
                                                                     const Quest& quest);

// A giver's hail as the game's own bark (notes.md 14 Oct 2026, In the Stocks: Benesh's
// words over his head, not on the dialogue's caption). q_<id>_hail.xml is a one-line
// `ingame monolog` dialogue on the giver -- the shape ambient-design/plan.md section 1.1
// proved on the Windows laptop -- under the dice-game role every ordinary soul holds (KOSTKAR_MUZ /
// KOSTKAR_ZENA by the giver's gender), its decision aliased so the graph's RequestMonologue
// can name it. The watch decides when (WatchHail) and tells the graph by a token of the
// hail's class (hail_class) in Henry's pocket for a moment, the pray's channel; the graph's
// ItemDescriptorTrigger on it, live while the quest is unasked, requests the monologue.
std::string hail_class(const Quest& quest);
std::string hail_name(const Quest& quest);
std::string hail_node(const std::string& quest_id);
std::string hail_alias(const std::string& modid, const std::string& quest_id);
std::string emit_hail_dialogue(const std::string& modid, const Quest& quest);

// Libs/Tables/item/item__<modid>.xml -- the mod's own item classes, for the things a
// favour is about that the shipped tables have no carriable row for (a saw). Empty when
// no quest names one, and then no file is written. The rows the inventory shows for them
// go in the same localization table as everything else.
std::string emit_item_table(const std::string& modid, const std::vector<Quest>& quests);
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
// game's own (IncludeModGraphs: "Quests/%s.xml" for a mod), a Project whose one
// definition is the Level below and whose one node is an instance of it, the shape of
// the shipped concept_test project (a Project, a Level under it, the quests under
// that). Definition paths are resolved against the project's own directory, so it
// names <modid>/<level>.xml from here.
std::string emit_dialogue_project(const std::string& modid);

// Data/Quests/<modid>/<kQuestLevel>.xml -- the Level the dialogue and the quests sit in,
// named for the game's own level (Libs/Tables/level.xml), which is what the journal
// files a quest's region by: under a bare Project every errand read "Different
// region" (noted 4 Oct 2026). It points at the dialogue definition beside it and, when
// there are quests, at a quest per file, with the edges from the dialogue's ports into
// them. Definition paths are resolved against this file's directory, so they name
// <kQuestLevel>/<file>.xml from here.
constexpr const char* kQuestLevel = "kutnohorsko";
std::string emit_quest_level(const std::string& modid,
                             const std::string& dialogue_name,
                             const std::vector<Quest>& quests);

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
constexpr double kFollowHoursAfter = 1.5;
constexpr double kFollowSeeMetres = 12.0;
constexpr double kFollowHearMetres = 4.0;
constexpr double kFollowConeDegrees = 120.0;
constexpr double kFollowEyeMetres = 1.6;
constexpr double kFollowWarnMetres = 25.0;
// Henry's `follow_over` goes up this long after the holder's Talk closes on the `followed`
// row: he has turned away, and the subtitle of his last line is gone.
constexpr int kFollowOverMs = 2500;
constexpr int kFollowRestHours = 11;
constexpr int kFollowClearHours = 6;
// The engine is asked for the holder's Talk this long after the Lua's verdict: time for him
// to turn and the token to land.
constexpr int kFollowTalkMs = 1500;
// How near Henry is before a travel target's face (seen_mood) is put on: an idle mood is
// subtle and reads at conversation distance, so it is on well before he is there.
constexpr int kTravelMoodMetres = 40;
// A giver with a hail written (npc_quest_lines `hail`) shouts it when Henry is this near
// with the errand unasked: shouting distance, far enough that he is calling Henry over
// rather than greeting him. Not again until Henry has been twice that far off and this
// many seconds have passed; the narrative subtitle holds the line this long (about what the
// widget gives a line on its own). The words go up this many milliseconds after the bark
// that turns him to Henry is started, since its opening wipes them (emit.cpp, WatchHail).
constexpr int kHailMetres = 15;
constexpr int kHailAgainSeconds = 90;
constexpr int kHailSeconds = 5;
constexpr int kHailWordsMs = 700;
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
// A find's keepsake (npcdb.hpp keepsake_class): laid at accept, and if that failed (the
// spot too far out to place at, a load that wiped it) laid again from the watch once Henry
// is within kKeepsakeMetres of the spot without it.
constexpr double kKeepsakeMetres = 120.0;
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
                                  const std::vector<Quest>& quests);

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
