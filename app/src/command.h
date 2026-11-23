// Freeform player command understanding and social-graph entity resolution.
//
// Decouples neural intent/mention extraction from database entity linking:
//   1. Stage 1 (`Stage1Gate`): scores the 768-d `EmbeddingGemma-300M` query vector with a
//      12 KB linear classifier (< 1 us) to skip ~85% of ordinary chat and biographical
//      knowledge questions ("Where was Margaret born?") at zero LLM cost.
//   2. Stage 2 (`Llm` sidecar, e.g. `Qwen3-0.6B-LoRA-SFT` Q4_K_M with GBNF grammar):
//      extracts `{action, target_mention}` without touching `Qwen3-4B`'s KV cache.
//   3. Stage 3 (`CommandGraph`): resolves `target_mention` (proper names, kinship roles,
//      trades, and multi-turn discourse pronouns) over the speaker's Hop-1 circle/kin/
//      absentees and Hop-2 bridges/quests using salience priors + Jaro-Winkler +
//      Bohemian/German/English phonetic matching.
//   4. Conditional STT Pass 2: when a spoken person/place command fails to resolve cleanly
//      on Pass 1, replays the buffered push-to-talk PCM through Moonshine STT with expanded
//      Hop-1 + Hop-2 + settlement keyterms before re-linking.

#ifndef MOONSHINE_COMMAND_H
#define MOONSHINE_COMMAND_H

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "embedder.h"
#include "llm.h"
#include "memory.h"
#include "personadb.h"
#include "prompt.h"
#include "scripted.h"

struct sqlite3;

namespace moonshine {

enum class CommandAction {
    None,
    LocateNpc,
    FollowNpc,
    TravelTo,
};

enum class CommandStatus {
    NoAction,
    ResolvedSoul,
    ResolvedPlace,
    UnmeetableAbsentee,
    GatedByTrust,
    UnknownToNpc,
    AmbiguousSoul,
};

struct CommandCandidate {
    std::string soul_name;
    std::string full_name;
    std::string given_name;
    std::string title;
    std::string byname;
    std::string ui_name;
    std::string gender;
    std::string settlement;
    std::string trade;
    std::vector<std::string> roles;
    int hop = 1;
    double closeness = 1.0;
    bool is_absentee = false;
    std::string why_unmeetable;
    std::string person_token;
    std::string via_name;
    std::vector<std::string> quest_ids;
};

struct SoulWhereabouts {
    std::string home_fact;
    std::string self_intro;
};

struct ExtractedCommand {
    CommandAction action = CommandAction::None;
    std::string target_mention;
    bool is_pronoun = false;
    std::string raw_output;
};

struct CommandOutcome {
    CommandAction action = CommandAction::None;
    CommandStatus status = CommandStatus::NoAction;
    std::string target_mention;
    std::string soul_name;
    std::string display_name;
    std::string settlement;
    std::string place_id;
    double confidence = 0.0;
    // Bracketed direction appended to the player's turn for the dialogue LLM (`Qwen3-4B`).
    std::string direction;
    // Turn-file directive (`@locate <soul> <len>:<spots>`, `@follow <soul>`, `@travel <id>`).
    std::string directive;
    // Person token to mark `@heard` when the NPC points Henry to someone new.
    std::string heard_token;
    // Status-line summary for debugging and telemetry.
    std::string note;
    bool pass2_used = false;
};

// GBNF grammar constraining compact sidecar output to `NONE | LOCATE: ... | FOLLOW: ... | TRAVEL: ...`.
extern const char* kCompactCommandGrammar;
extern const char* kCompactCommandSystemPrompt;

// Parses either compact sidecar output (`LOCATE: Margaret`) or FunctionGemma call syntax.
ExtractedCommand parse_extracted_command(std::string_view raw);

// Bohemian/German/English phonetic skeleton and Jaro-Winkler similarity, exposed for tests.
std::string command_normalize(std::string_view text);
std::string command_phonetic_key(std::string_view word);
double jaro_winkler_similarity(std::string_view a, std::string_view b);

// Formats `@locate <soul> <len>:<spots_payload>` for `Moonshine.Directives` in `emit.cpp`.
std::string format_locate_directive(std::string_view soul,
                                    const std::vector<RoutineSpot>& spots);

// Normalizes a spoken or typed reply to "Who are you looking for?" so bare names
// ("Benesh Bell"), trades ("the blacksmith"), and full sentences ("I'm looking for
// Benesh Bell") all feed in cleanly as the extracted `locate_npc` subject.
std::string clean_guard_subject_mention(std::string_view raw);

// Stage-1 768-d linear intent gate trained on `EmbeddingGemma-300M` vectors.
class Stage1Gate {
public:
    Stage1Gate() = default;
    static std::optional<Stage1Gate> load_json(const std::filesystem::path& path);

    bool loaded() const { return dim_ > 0 && weights_.size() == static_cast<std::size_t>(4 * dim_); }
    // Probability in [0, 1] that unit vector `q` is a command (`1 - P(no_action)`).
    float command_probability(const std::vector<float>& q) const;
    bool should_run_extractor(const std::vector<float>& q) const;
    float threshold() const { return threshold_; }
    void set_threshold(float t) { threshold_ = t; }

private:
    int dim_ = 0;
    float threshold_ = 0.65f;
    std::vector<float> weights_;  // 4 * dim_ row-major
    std::vector<float> bias_;     // 4
};

// Read-only social-graph candidate loader over `npcs.sqlite` (with in-memory `hold()` for tests).
class CommandGraph {
public:
    CommandGraph() = default;
    explicit CommandGraph(std::filesystem::path db_path);
    ~CommandGraph();

    CommandGraph(const CommandGraph&) = delete;
    CommandGraph& operator=(const CommandGraph&) = delete;

    bool ok() const { return db_ != nullptr || !held_.empty() || !held_locations_.empty(); }
    const std::string& trouble() const { return trouble_; }

    void hold(std::string speaker_soul,
              std::vector<CommandCandidate> candidates,
              std::map<std::string, std::vector<RoutineSpot>> spots = {});

    void hold_location(std::string location_id,
                       std::vector<CommandCandidate> candidates,
                       std::map<std::string, std::vector<RoutineSpot>> spots = {});

    std::vector<CommandCandidate> candidates_for(std::string_view speaker_soul);
    std::vector<CommandCandidate> candidates_in_location(std::string_view location_id);
    std::vector<RoutineSpot> spots_of(std::string_view target_soul);
    SoulWhereabouts whereabouts_of(std::string_view target_soul);

private:
    std::vector<CommandCandidate> query_db_candidates(std::string_view speaker_soul) const;
    std::vector<CommandCandidate> query_db_location_candidates(std::string_view location_id) const;
    std::vector<RoutineSpot> query_db_spots(std::string_view target_soul) const;
    SoulWhereabouts query_db_whereabouts(std::string_view target_soul) const;

    std::filesystem::path path_;
    sqlite3* db_ = nullptr;
    std::string trouble_;
    std::mutex lock_;
    std::map<std::string, std::vector<CommandCandidate>, std::less<>> cache_;
    std::map<std::string, std::vector<CommandCandidate>, std::less<>> location_cache_;
    std::map<std::string, std::vector<CommandCandidate>, std::less<>> held_;
    std::map<std::string, std::vector<CommandCandidate>, std::less<>> held_locations_;
    std::map<std::string, std::vector<RoutineSpot>, std::less<>> held_spots_;
};

class CommandDirector {
public:
    CommandDirector(Embedder* embedder,
                    Llm* extractor_llm,
                    CommandGraph& graph,
                    Stage1Gate gate = {},
                    Options extractor_options = {});

    // Comma-separated salience-ordered proper names for proactive Moonshine Pass-1 biasing.
    std::string pass1_keyterms(std::string_view speaker_soul,
                               const std::vector<Message>& history,
                               const SoulRecord& record,
                               std::size_t max_terms = 45);

    // Expanded Hop-1 + Hop-2 + canonical settlement keyterms for conditional Pass-2 replay.
    std::string pass2_keyterms(std::string_view speaker_soul,
                               const std::vector<Message>& history,
                               const SoulRecord& record,
                               std::size_t max_terms = 95);

    // Keyterms drawn from everyone inside a Burgher (`SettlementLocation`) population center,
    // ranked toward `query_hint` when re-transcribing a spoken guard lookup.
    std::string location_keyterms(std::string_view location_id,
                                  std::size_t max_terms = 95,
                                  std::string_view query_hint = {});

    // Deterministic entity resolution given an already-extracted `{action, target_mention}`.
    CommandOutcome link(std::string_view speaker_soul,
                        std::string_view speaker_name,
                        const ExtractedCommand& extracted,
                        const std::vector<Message>& history,
                        const SoulRecord& record,
                        KbGate kb_gate = KbGate::Open,
                        std::string_view guard_location = {});

    // Direct subject resolution after "Can you help me find someone?" -> "Who are you looking
    // for?", treating spoken or typed input directly as the `locate_npc` subject over the
    // guard's Burgher population center (with optional Pass-2 STT replay).
    CommandOutcome resolve_guard_lookup(
        std::string_view guard_soul,
        std::string_view guard_name,
        std::string_view guard_location,
        std::string_view said,
        const std::vector<Message>& history,
        const SoulRecord& record,
        const std::function<std::string(std::string_view)>& retranscribe_fn = {});

    // Full Stage-1 -> Stage-2 -> Stage-3 (+ optional conditional STT Pass-2) pipeline.
    CommandOutcome resolve(std::string_view speaker_soul,
                           std::string_view speaker_name,
                           std::string_view speaker_settlement,
                           std::string_view said,
                           const std::vector<Message>& history,
                           const SoulRecord& record,
                           KbGate kb_gate = KbGate::Open,
                           const std::function<std::string(std::string_view)>& retranscribe_fn = {},
                           std::string_view guard_location = {});

private:
    ExtractedCommand run_extractor(std::string_view speaker_name,
                                   std::string_view speaker_settlement,
                                   std::string_view said,
                                   const std::vector<Message>& history);

    Embedder* embedder_ = nullptr;
    Llm* extractor_ = nullptr;
    CommandGraph& graph_;
    Stage1Gate gate_;
    Options extractor_options_;
    std::mutex llm_lock_;
};

}  // namespace moonshine

#endif  // MOONSHINE_COMMAND_H
