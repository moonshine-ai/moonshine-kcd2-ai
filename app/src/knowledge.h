// What a soul knows, found for whatever the player typed or said, and handed to the model
// as a direction on the player's turn.
//
// The generator wrote every soul a knowledge base -- their life, their views, their town,
// the news, the weather, the tales they have heard, what their trade knows -- with a vector
// for every answer and for every way the player might ask (data/kcd2/knowledge-design/
// plan.md). The menu reaches a little of it through written rows; this is the road for a
// line the menu did not offer. The rule the plan settled on (section 7.2) and the loop
// measured outside the app (section 6b):
//
//   score the line against the soul's vectors; take the best above tau, except that a hub
//   (`tell me a story`) loses to a specific item close behind it and an item the player
//   is not trusted with loses to an open one close behind; inject what she knows, or that
//   she will not say yet, or that she knows nothing, as a bracketed direction the model is
//   told to use only if it answers what was asked.
//
// Two things come before the knowledge. A line that amounts to "do you have a quest?" is
// answered with the giver's own ask, since a player offering help should hear the errand
// and not a fact about the weather. And a short line after a reply that offered a choice
// ("please do", "the other one", "why?") is read against that reply rather than on its own,
// which is how an NPC's question gets its answer heard.
//
// Nothing here goes into the system prompt. The sheet has to stand still for the whole
// conversation so the cache is never fought over (prompt.h), and so the knowledge rides the
// player's turn, as the quest director's direction does (quest.h).

#ifndef MOONSHINE_KNOWLEDGE_H
#define MOONSHINE_KNOWLEDGE_H

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "embedder.h"
#include "scripted.h"

struct sqlite3;

namespace moonshine {

// The thresholds below are scores against the stored vectors: EmbeddingGemma's 768
// components as int8 with a scale per vector (data/schema.sql, npcs-vectors.sqlite). The
// quantisation does not move them: over 8 souls x 19 lines the int8 rows and the float32
// ones agreed on the best item every time and on every shared score to within 0.002, so
// the calibrations below, made at float32, stand. (The first 256 components alone were
// tried and did not: scores ran 0.77x + 0.20 and the best item changed on one line in ten.)
//
// Below this the best item is not what the line was about, and nothing is injected.
// Measured on the pilot's gold set with the answer-and-questions index (plan section 6a):
// keeps 95% of what is in scope and turns away 96% of what is not.
inline constexpr float kKnowledgeTau = 0.51f;
// A hub item -- the opener that offers two or three of a topic's rows -- wins only when no
// specific item is within this of it (section 6, point 5).
inline constexpr float kHubMargin = 0.03f;
// An item the player is not trusted with loses to an open, specific one within this of it,
// so a near tie goes to the thing she can say rather than the thing she will not.
inline constexpr float kGateMargin = 0.03f;
// A line of this many words or fewer, with nothing above tau, is read against her last reply.
inline constexpr int kFollowWords = 7;
// ...and the best of what that reply offered wins if it clears this.
inline constexpr float kFollowTau = 0.35f;
// Above this the line is the player offering to help, and the quest comes first.
inline constexpr float kQuestAskTau = 0.62f;

// One thing a soul knows, as npc_kb_item has it. `trust` is open, friendly or confided, the
// same tiers scripted.h gates the menu's topics by; `withheld` is what she says instead
// when asked too soon; `price_what` the favour that would open it a tier early.
struct KbItem {
    std::string owner;    // the soul, or the shared set the item came from
    std::string item_id;  // "life:birth", "tale:3", "hub:tales", "town:smith" ...
    std::string scope;
    std::string topic;
    std::string kind;     // fact, rumour, story, opinion, chat, taboo, hub
    std::string label;
    std::string trust;
    std::string price_what;
    std::string withheld;
    std::string answer;
    std::string flag;     // the build's doubt about the answer; non-empty means say it plainly

    bool is_hub() const { return item_id.rfind("hub:", 0) == 0; }
    bool is_tale() const { return item_id.rfind("tale:", 0) == 0; }
    bool is_taboo() const { return kind == "taboo"; }
};

// An item scored against a line: the item, its best score, and which of its texts scored
// it (the answer, or one of the generated questions).
struct KbHit {
    const KbItem* item = nullptr;
    float score = 0.0f;
    std::string via;
};

// A soul's items with their vectors, built once when the conversation opens and queried
// per line: a few hundred items, a few thousand vectors, a matrix multiply of a few
// milliseconds.
class SoulIndex {
public:
    // An item and the vectors of its texts, the answer's first. Vectors of another length
    // than the first one added are dropped, since they are from another model.
    void add(KbItem item, std::vector<std::vector<float>> vectors,
             std::vector<std::string> texts);

    // The items ranked for a unit query vector, best first, each with its best text's
    // score. A query wider than the stored vectors is cut to their width (truncated(),
    // embedder.h); a narrower one is another model's and scores nothing. `prefixes`
    // restricts the field to items whose id starts with one of them; empty means all. At
    // most `top` of them.
    std::vector<KbHit> query(const std::vector<float>& q,
                             const std::vector<std::string_view>& prefixes = {},
                             std::size_t top = 12) const;

    const KbItem* find(std::string_view item_id) const;
    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    int dim() const { return dim_; }
    std::size_t vectors() const { return rows_.size(); }

private:
    std::vector<std::unique_ptr<KbItem>> items_;
    struct Row {
        std::size_t item;
        std::string text;
    };
    std::vector<Row> rows_;
    std::vector<float> mat_;  // rows_.size() * dim_, row-major
    int dim_ = 0;
};

// What a hub item offers, by the id prefix of the items it spans. "Tell me a story" is
// answered by the hub naming two or three tales; "please do" then picks among the tales.
std::vector<std::string_view> hub_span(std::string_view hub_id);

// The item the app injects, by the rule above, or nothing when the best is below tau.
// `tier` is the player's standing with the soul (scripted.h).
std::optional<KbHit> pick(const std::vector<KbHit>& hits, Trust tier);

// Whether the player's line is them offering to help -- the plainest phrasings, by pattern,
// for when the embedder is not there or scores it low. The embedder's say is kQuestAskTau
// against the asks in kQuestAsks.
bool reads_as_quest_ask(std::string_view line);
// Ways of offering to help, embedded under the document prompt and scored against the line.
extern const std::vector<std::string_view> kQuestAsks;

// Whether a short line reads as asking for another of what was offered rather than for more
// of the same ("the other one", "what about the second").
bool asks_for_another(std::string_view line);

// A reply the NPC gave that drew on an item, kept so a short line after it can be read in
// its light. The session keeps the last few.
struct Drew {
    std::string item_id;  // empty for a reply that drew on nothing
    std::string reply;
};

// What the session adds to the player's words for the model, and a note for the status line.
struct Advice {
    std::string direction;  // bracketed; empty means nothing to add
    std::string note;
    std::string item_id;    // the item the direction carries, "quest:<id>", or empty
    // The reply is a tale told whole, which wants more tokens than a line does.
    bool tale = false;
    // The direction gives them something she knows, or withholds it for want of trust: what
    // a giver's gate (KbGate) holds back until their favour is done. A taboo's shock and
    // honest ignorance are not knowledge given, and go out whatever the gate.
    bool knows = false;
};

// The direction for a hit, or for no hit, at the player's standing (section 6b: what she
// knows; that she will not say yet, with the withheld line and the price; the taboo's shock
// with the gesture stripped; the follow-up's "more" or "chose"). `followup` is empty, "more"
// or "chose". Written to the character in the second person, as the quest director's are.
Advice direction_for_hit(const KbHit* hit, Trust tier, std::string_view followup = {});

// Where the giver's favour stands when the player offers to help: there is none; it is there
// to be asked; it has been put to them this conversation already; or it is no longer there
// to be asked -- taken up, or done -- and the sheet says where it stands.
enum class AskState { None, Fresh, Again, Settled };

// The direction for a line that offers help, with the giver's ask (the offer text, the
// reward in groschen or 0) and where it stands.
Advice quest_ask_direction(std::string_view quest_id, std::string_view ask, int reward_groschen,
                           AskState state);

// The advice for a line, after the giver's gate: an `advice` that gives knowledge while the
// gate is not open is replaced by the giver putting them off -- and, with the favour there to
// be asked, asking it (`ask`, the offer; `again` when it has been put this conversation
// already), or, with it taken up, reminding them of it (`remind`). Anything else -- ignorance,
// a taboo, the quest's own direction -- passes through. The direction says the knowledge is
// the help the favour pays for, so the player learns what the gate is.
Advice gate_knowledge(Advice advice, KbGate gate, std::string_view quest_id, std::string_view ask,
                      bool again, std::string_view remind);

// The knowledge tables of npcs.sqlite, read-only, with the vectors from npcs-vectors.sqlite
// beside it (attached as `vec`; data/schema.sql says why they are two files). A database
// from before the split, with npc_kb_vector in the one file, is read as it was.
class KnowledgeBase {
public:
    explicit KnowledgeBase(std::filesystem::path path);
    ~KnowledgeBase();
    KnowledgeBase(const KnowledgeBase&) = delete;
    KnowledgeBase& operator=(const KnowledgeBase&) = delete;

    bool ok() const { return db_ != nullptr; }
    const std::string& trouble() const { return trouble_; }
    // Whether the tables are there at all: a database built before the knowledge stage has
    // every soul knowing nothing, which is worth one line at start-up rather than a line a
    // conversation. False too when the vectors' file is missing, which trouble() then says.
    bool has_tables() const;
    // Where the vectors are read from: "vec" (npcs-vectors.sqlite) or "main" (the one-file
    // layout), for the status line.
    const std::string& vectors_in() const { return vectors_in_; }
    // How many souls have items, for the status line.
    int souls_with_items() const;
    // Everything the soul's owners hold, with the vectors of every answer and question that
    // has one. Empty for a soul with no owner row.
    SoulIndex load(std::string_view soul) const;

private:
    std::filesystem::path path_;
    sqlite3* db_ = nullptr;
    std::string trouble_;
    std::string vectors_in_;
};

// The director for the knowledge: the embedder, the base, and the index of the soul being
// talked to.
class KnowledgeDirector {
public:
    KnowledgeDirector(Embedder& embedder, KnowledgeBase& base);

    // Reads the soul's items in, so the first line of the conversation does not pay for it.
    // Returns how many items they have.
    std::size_t open(std::string_view soul);
    // An index supplied rather than read from the base, for a soul whose items come from
    // elsewhere -- the tests, or a playground with its own.
    void hold(std::string soul, SoulIndex index);
    std::size_t items_of(std::string_view soul) const;

    // Whether the line is the player offering to help: by pattern, or by its nearest of
    // kQuestAsks clearing kQuestAskTau. The score is returned for the note.
    bool is_quest_ask(std::string_view line, float* score = nullptr);

    // The direction for the line, at the player's standing, given the replies so far that
    // drew on items (newest last). Retrieval, then the follow-up reading when nothing scored,
    // then the words.
    Advice advise(std::string_view soul, std::string_view line, Trust tier,
                  const std::vector<Drew>& drew);

    // The soul's items ranked for a line, for inspection.
    std::vector<KbHit> rank(std::string_view soul, std::string_view line);

    Embedder& embedder() { return embedder_; }

private:
    const SoulIndex& index_of(std::string_view soul);
    void ensure_asks();

    Embedder& embedder_;
    KnowledgeBase& base_;
    std::mutex lock_;
    // A few souls' indexes, since the player walks between the same handful; dropped oldest
    // first beyond that.
    std::map<std::string, SoulIndex, std::less<>> indexes_;
    std::vector<std::string> order_;
    std::vector<std::vector<float>> ask_vectors_;
};

}  // namespace moonshine

#endif  // MOONSHINE_KNOWLEDGE_H
