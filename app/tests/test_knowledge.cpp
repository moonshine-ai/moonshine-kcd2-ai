#include "doctest.h"

#include <cmath>
#include <string>
#include <vector>

#include "knowledge.h"

using namespace moonshine;

namespace {

// Unit vectors along the axes, so a test can say exactly how close two things are: an item
// "at" axis k scores 1.0 against a query at k and 0.0 against any other, and a query that is
// a mix of two axes scores each by its weight.
std::vector<float> axis(std::size_t k, std::size_t dim = 8) {
    std::vector<float> v(dim, 0.0f);
    v[k] = 1.0f;
    return v;
}

std::vector<float> mix(std::size_t a, float wa, std::size_t b, float wb, std::size_t dim = 8) {
    std::vector<float> v(dim, 0.0f);
    v[a] = wa;
    v[b] = wb;
    const float norm = std::sqrt(wa * wa + wb * wb);
    for (float& x : v) x /= norm;
    return v;
}

KbItem item(std::string id, std::string trust = "open", std::string kind = "fact",
            std::string answer = "an answer") {
    KbItem out;
    out.owner = "soul";
    out.item_id = std::move(id);
    out.trust = std::move(trust);
    out.kind = std::move(kind);
    out.answer = std::move(answer);
    return out;
}

KbHit hit(const KbItem& it, float score) { return {&it, score, "text"}; }

}  // namespace

TEST_CASE("the index scores an item by its best text and ranks the items") {
    SoulIndex index;
    // Two items: the first's answer is at axis 0 and one of its questions at axis 1; the
    // second is at axis 2 only.
    index.add(item("life:birth"), {axis(0), axis(1)}, {"born in Malin", "where were you born"});
    index.add(item("town:smith"), {axis(2)}, {"the smith is by the gate"});
    CHECK(index.size() == 2);
    CHECK(index.vectors() == 3);

    // A query at axis 1 finds the first item through its question.
    std::vector<KbHit> hits = index.query(axis(1));
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].item->item_id == "life:birth");
    CHECK(hits[0].score == doctest::Approx(1.0f));
    CHECK(hits[0].via == "where were you born");
    CHECK(hits[1].item->item_id == "town:smith");
    CHECK(hits[1].score == doctest::Approx(0.0f));

    // A prefix narrows the field.
    hits = index.query(axis(1), {"town:"});
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].item->item_id == "town:smith");

    CHECK(index.find("town:smith") != nullptr);
    CHECK(index.find("town:baker") == nullptr);
}

TEST_CASE("a vector of the wrong length is dropped rather than scored") {
    SoulIndex index;
    index.add(item("a"), {axis(0, 8)}, {"a"});
    index.add(item("b"), {axis(0, 4)}, {"b"});  // another model's vector
    CHECK(index.vectors() == 1);
    CHECK(index.query(axis(0, 8)).size() == 1);
    CHECK(index.query(axis(0, 4)).empty());
}

TEST_CASE("a query wider than the stored vectors is cut to their width, a unit vector again") {
    SoulIndex index;
    index.add(item("a"), {axis(0, 4)}, {"a"});
    index.add(item("b"), {axis(1, 4)}, {"b"});
    // (0.6, 0.8) in the first two components and weight beyond them that is cut away: the
    // kept part re-normalised is (0.6, 0.8, 0, 0), so b scores 0.8 and a 0.6.
    const std::vector<KbHit> hits = index.query({0.3f, 0.4f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f});
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].item->item_id == "b");
    CHECK(hits[0].score == doctest::Approx(0.8f));
    CHECK(hits[1].score == doctest::Approx(0.6f));
    // truncated() itself: a vector no wider than asked comes back untouched.
    CHECK(truncated(axis(1, 4), 8) == axis(1, 4));
    CHECK(truncated(axis(1, 8), 4) == axis(1, 4));
}

TEST_CASE("pick takes the best above tau and nothing below it") {
    const KbItem a = item("life:birth");
    const KbItem b = item("life:trade");
    CHECK_FALSE(pick({hit(a, 0.50f), hit(b, 0.40f)}, Trust::Open).has_value());
    const auto chosen = pick({hit(a, 0.70f), hit(b, 0.60f)}, Trust::Open);
    REQUIRE(chosen.has_value());
    CHECK(chosen->item->item_id == "life:birth");
}

TEST_CASE("a hub loses to a specific item close behind it, and only then") {
    const KbItem hub = item("hub:tales", "open", "hub");
    const KbItem tale = item("tale:3", "open", "story");
    // Within the margin: the tale answers "know any tales about a kidnapper" better than
    // the offer of tales does.
    auto chosen = pick({hit(hub, 0.56f), hit(tale, 0.55f)}, Trust::Open);
    REQUIRE(chosen.has_value());
    CHECK(chosen->item->item_id == "tale:3");
    // Well ahead: "tell me a story" gets the offer.
    chosen = pick({hit(hub, 0.60f), hit(tale, 0.52f)}, Trust::Open);
    REQUIRE(chosen.has_value());
    CHECK(chosen->item->item_id == "hub:tales");
}

TEST_CASE("an item the player is not trusted with loses a near tie to an open one") {
    const KbItem gated = item("life:marriage", "friendly");
    const KbItem open = item("life:trade", "open");
    const KbItem hub = item("hub:life", "open", "hub");
    // A stranger: the open item within the margin is said instead.
    auto chosen = pick({hit(gated, 0.60f), hit(open, 0.58f)}, Trust::Open);
    REQUIRE(chosen.has_value());
    CHECK(chosen->item->item_id == "life:trade");
    // But not when the open one is a hub, which is an offer and not an answer.
    chosen = pick({hit(gated, 0.60f), hit(hub, 0.58f)}, Trust::Open);
    REQUIRE(chosen.has_value());
    CHECK(chosen->item->item_id == "life:marriage");
    // Nor when the open one is too far behind: the gated item stands, and is withheld.
    chosen = pick({hit(gated, 0.60f), hit(open, 0.50f)}, Trust::Open);
    REQUIRE(chosen.has_value());
    CHECK(chosen->item->item_id == "life:marriage");
    // Somebody trusted gets the gated item outright.
    chosen = pick({hit(gated, 0.60f), hit(open, 0.58f)}, Trust::Friendly);
    REQUIRE(chosen.has_value());
    CHECK(chosen->item->item_id == "life:marriage");
}

TEST_CASE("the direction says what is known, in the second person, and keeps the answer") {
    const KbItem fact = item("town:smith", "open", "fact", "The smith is by the Kourim gate.");
    const KbHit h = hit(fact, 0.7f);
    const Advice advice = direction_for_hit(&h, Trust::Open);
    CHECK(advice.direction.front() == '[');
    CHECK(advice.direction.back() == ']');
    CHECK(advice.direction.find("What you know about this: The smith is by the Kourim gate.") != std::string::npos);
    CHECK(advice.item_id == "town:smith");
    CHECK(advice.note == "used town:smith");
    CHECK_FALSE(advice.tale);
}

TEST_CASE("nothing above tau is honest ignorance, but greetings are left alone") {
    const Advice advice = direction_for_hit(nullptr, Trust::Open);
    CHECK(advice.direction.find("you know nothing of it") != std::string::npos);
    CHECK(advice.direction.find("small talk") != std::string::npos);
    CHECK(advice.item_id.empty());
    CHECK(advice.note == "nothing above tau");
}

TEST_CASE("a gated item is withheld with the price and the written deflection") {
    KbItem gated = item("life:money", "confided", "fact", "I have forty groschen buried.");
    gated.withheld = "That is my business and no stranger's.";
    gated.price_what = "a jug of ale";
    const KbHit h = hit(gated, 0.7f);
    const Advice advice = direction_for_hit(&h, Trust::Friendly);
    CHECK(advice.direction.find("will not tell them yet") != std::string::npos);
    CHECK(advice.direction.find("a jug of ale") != std::string::npos);
    CHECK(advice.direction.find("That is my business") != std::string::npos);
    // The answer itself is not given to the model to let slip.
    CHECK(advice.direction.find("forty groschen") == std::string::npos);
    CHECK(advice.note == "withheld life:money (confided)");
    // Trusted, the same item is said.
    const Advice told = direction_for_hit(&h, Trust::Confided);
    CHECK(told.direction.find("forty groschen") != std::string::npos);
}

TEST_CASE("a giver's knowledge is held back until the favour is done: asked, owed, or shut") {
    const KbItem fact = item("town:smith", "open", "fact", "The smith is by the Kourim gate.");
    const KbHit h = hit(fact, 0.7f);
    const Advice known = direction_for_hit(&h, Trust::Open);
    CHECK(known.knows);
    const std::string ask = "Julian has my axe. Bring it back and I'll help you however I can.";

    // Open: as it was.
    CHECK(gate_knowledge(known, KbGate::Open, "q1", ask, false, "").direction == known.direction);

    // There to be asked: the fact is not given, the ask is put, and the direction carries the
    // quest so the judge's yes and the second asking follow as for an offer of help.
    const Advice asked = gate_knowledge(known, KbGate::Asking, "q1", ask, false, "");
    CHECK(asked.direction.find("Kourim") == std::string::npos);
    CHECK(asked.direction.find("once they have helped you") != std::string::npos);
    CHECK(asked.direction.find(ask) != std::string::npos);
    CHECK(asked.direction.find("do not assume they have agreed") != std::string::npos);
    CHECK(asked.item_id == "quest:q1");
    CHECK_FALSE(asked.knows);
    CHECK(asked.note == "gated (asking), would have used town:smith");
    // Put already this conversation: a reminder, not the whole offer again.
    const Advice again = gate_knowledge(known, KbGate::Asking, "q1", ask, true, "");
    CHECK(again.direction.find("asked them for help already") != std::string::npos);
    CHECK(again.direction.find(ask) == std::string::npos);
    CHECK(again.item_id == "quest:q1");

    // Taken up: the reminder, with what was agreed.
    const Advice owed = gate_knowledge(known, KbGate::Owed, "q1", ask, false, "Julian has my axe.");
    CHECK(owed.direction.find("your errand in hand") != std::string::npos);
    CHECK(owed.direction.find("Julian has my axe.") != std::string::npos);
    CHECK(owed.direction.find("Kourim") == std::string::npos);
    CHECK(owed.item_id.empty());
    const Advice owed_unwritten = gate_knowledge(known, KbGate::Owed, "q1", ask, false, "");
    CHECK(owed_unwritten.direction.find("in a word") != std::string::npos);
    CHECK(owed_unwritten.direction.back() == ']');

    // Not theirs to ask yet: put off, nothing asked.
    const Advice shut = gate_knowledge(known, KbGate::Shut, "q1", ask, false, "");
    CHECK(shut.direction.find("no mind for a stranger's questions") != std::string::npos);
    CHECK(shut.direction.find(ask) == std::string::npos);
    CHECK(shut.item_id.empty());

    // What is not knowledge given passes the gate: ignorance, a taboo, and a withheld item
    // is knowledge too and is gated with the rest.
    const Advice nothing = direction_for_hit(nullptr, Trust::Open);
    CHECK_FALSE(nothing.knows);
    CHECK(gate_knowledge(nothing, KbGate::Asking, "q1", ask, false, "").direction == nothing.direction);
    const KbItem taboo = item("taboo:god", "open", "taboo", "God forgive you for asking.");
    const KbHit t = hit(taboo, 0.7f);
    const Advice shocked = direction_for_hit(&t, Trust::Open);
    CHECK_FALSE(shocked.knows);
    CHECK(gate_knowledge(shocked, KbGate::Owed, "q1", ask, false, "").direction == shocked.direction);
    const KbItem gated = item("life:money", "confided", "fact", "I have forty groschen buried.");
    const KbHit g = hit(gated, 0.7f);
    CHECK(direction_for_hit(&g, Trust::Open).knows);
}

TEST_CASE("a taboo shuts the question down with the gesture stripped") {
    const KbItem taboo = item("taboo:god", "open", "taboo", "[crosses himself] God forgive you for asking.");
    const KbHit h = hit(taboo, 0.7f);
    const Advice advice = direction_for_hit(&h, Trust::Open);
    CHECK(advice.direction.find("God forgive you for asking.") != std::string::npos);
    CHECK(advice.direction.find("crosses himself") == std::string::npos);
    CHECK(advice.direction.find("[crosses") == std::string::npos);
    CHECK(advice.note == "taboo taboo:god");
}

TEST_CASE("a tale is told whole, with its length, and flagged for the longer reply") {
    const KbItem tale = item("tale:2", "open", "story", "Once a monk found silver in a hill and told the lord of it.");
    const KbHit h = hit(tale, 0.7f);
    const Advice advice = direction_for_hit(&h, Trust::Open);
    CHECK(advice.tale);
    CHECK(advice.direction.find("whole, start to end") != std::string::npos);
    CHECK(advice.direction.find("(14 words)") != std::string::npos);
    CHECK(advice.direction.find("Once a monk found silver") != std::string::npos);
    // Chosen from an offer, it says so.
    const Advice chosen = direction_for_hit(&h, Trust::Open, "chose");
    CHECK(chosen.direction.find("They have chosen this from what you offered") != std::string::npos);
    CHECK(chosen.note == "chose tale:2");
}

TEST_CASE("more on the same item says that is all there is") {
    const KbItem fact = item("news:roof", "open", "fact", "The roof fell in at the Krejci house.");
    const KbHit h = hit(fact, 0.0f);
    const Advice advice = direction_for_hit(&h, Trust::Open, "more");
    CHECK(advice.direction.find("All you know of it is still this") != std::string::npos);
    CHECK(advice.direction.find("nothing new") != std::string::npos);
    CHECK(advice.note == "more on news:roof");
}

TEST_CASE("the quest ask is put plainly, once, and the reward never as a figure") {
    Advice fresh = quest_ask_direction("q1", "My axe was taken by the carter. Bring it back.", 40, AskState::Fresh);
    CHECK(fresh.direction.find("offering to help, and you have something to ask") != std::string::npos);
    CHECK(fresh.direction.find("My axe was taken by the carter.") != std::string::npos);
    CHECK(fresh.direction.find("money in it") != std::string::npos);
    CHECK(fresh.direction.find("40") == std::string::npos);
    CHECK(fresh.item_id == "quest:q1");

    const Advice again = quest_ask_direction("q1", "My axe was taken.", 40, AskState::Again);
    CHECK(again.direction.find("offering to help again") != std::string::npos);
    CHECK(again.note == "quest q1 (again)");

    const Advice settled = quest_ask_direction("q1", "My axe was taken.", 40, AskState::Settled);
    CHECK(settled.direction.find("no new errand") != std::string::npos);
    CHECK(settled.direction.find("My axe") == std::string::npos);

    const Advice none = quest_ask_direction("", "", 0, AskState::None);
    CHECK(none.direction.find("no task to give them") != std::string::npos);
    CHECK(none.item_id.empty());
}

TEST_CASE("the plainest offers of help are read by pattern") {
    CHECK(reads_as_quest_ask("What ails you?"));
    CHECK(reads_as_quest_ask("Is there anything I can help you with?"));
    CHECK(reads_as_quest_ask("anything else I can do for you"));
    CHECK(reads_as_quest_ask("I'm looking for work"));
    CHECK(reads_as_quest_ask("what's the matter"));
    CHECK_FALSE(reads_as_quest_ask("Where is the smith?"));
    CHECK_FALSE(reads_as_quest_ask("Tell me a story."));
    CHECK_FALSE(reads_as_quest_ask("Help me understand the weather."));
}

TEST_CASE("asking for another of what was offered is told from asking for more") {
    CHECK(asks_for_another("and the other one?"));
    CHECK(asks_for_another("what about the second"));
    CHECK(asks_for_another("another"));
    CHECK_FALSE(asks_for_another("why?"));
    CHECK_FALSE(asks_for_another("please do"));
}

TEST_CASE("a hub spans the items it offers") {
    CHECK(hub_span("hub:tales") == std::vector<std::string_view>{"tale:"});
    CHECK(hub_span("hub:life") == std::vector<std::string_view>{"life:"});
    CHECK(hub_span("hub:times") == std::vector<std::string_view>{"view:"});
    CHECK(hub_span("life:birth").empty());
    CHECK(hub_span("").empty());
}

// The director over a stub embedder, which maps a few known lines to axes so the follow-up
// reading can be exercised: a line nothing is written for, the hub's offer, the two tales.
namespace {

class StubEmbedder : public Embedder {
public:
    std::vector<float> embed(std::string_view text, bool) override {
        ++calls;
        const std::string line(text);
        // The hub's reply names both tales and the player answers it: "please do" takes the
        // one the reply led with; "the other one" leans to the second.
        if (line.find("Would you hear of the kidnapper") != std::string::npos) {
            return line.find("please do") != std::string::npos ? mix(1, 0.8f, 2, 0.6f)
                                                                : mix(1, 0.6f, 2, 0.8f);
        }
        if (line.find("tell me a story") != std::string::npos) return axis(0);
        if (line.find("kidnapper") != std::string::npos) return axis(1);
        if (line.find("silver") != std::string::npos) return axis(2);
        if (line.find("smith") != std::string::npos) return axis(3);
        return axis(7);  // nothing near anything
    }
    int dim() const override { return 8; }
    std::string describe() const override { return "stub"; }
    int calls = 0;
};

// A base over a database that is not there, so the director's own index is what is used.
struct DirectorFixture {
    StubEmbedder embedder;
    KnowledgeBase base{"/nonexistent/npcs.sqlite"};
    KnowledgeDirector director{embedder, base};
};

}  // namespace

TEST_CASE("without a base the director adds no direction at all") {
    DirectorFixture f;
    CHECK_FALSE(f.base.ok());
    CHECK(f.director.open("kkut_man_1") == 0);
    const Advice advice = f.director.advise("kkut_man_1", "where is the smith", Trust::Open, {});
    CHECK(advice.direction.empty());
    CHECK(advice.note == "no knowledge base for this soul");
}

namespace {

// A soul with a hub of tales, two tales and a fact, held in the director.
void with_tales(KnowledgeDirector& director) {
    SoulIndex index;
    index.add(item("hub:tales", "open", "hub", "Would you hear of the kidnapper, or of the monk and the silver?"),
              {axis(0)}, {"tell me a story"});
    index.add(item("tale:1", "open", "story", "A kidnapper took the miller's child and was found out by a dog."),
              {axis(1)}, {"the kidnapper"});
    index.add(item("tale:2", "open", "story", "A monk found silver in the hill above Kutna Hora."),
              {axis(2)}, {"the silver"});
    index.add(item("town:smith", "open", "fact", "The smith is by the gate."), {axis(3)}, {"smith"});
    director.hold("soul", std::move(index));
}

}  // namespace

TEST_CASE("a line above tau is answered with the item, and a hub with its offer") {
    DirectorFixture f;
    with_tales(f.director);
    Advice advice = f.director.advise("soul", "where is the smith", Trust::Open, {});
    CHECK(advice.item_id == "town:smith");
    CHECK(advice.direction.find("The smith is by the gate.") != std::string::npos);
    advice = f.director.advise("soul", "tell me a story", Trust::Open, {});
    CHECK(advice.item_id == "hub:tales");
    CHECK_FALSE(advice.tale);
    // Asked for the kidnapper by name, the tale itself and not the offer.
    advice = f.director.advise("soul", "know a tale about a kidnapper?", Trust::Open, {});
    CHECK(advice.item_id == "tale:1");
    CHECK(advice.tale);
}

TEST_CASE("'please do' after the hub's offer picks the tale the reply led with") {
    DirectorFixture f;
    with_tales(f.director);
    const std::vector<Drew> drew = {
        {"", "Hm? What do you want?"},
        {"hub:tales", "Would you hear of the kidnapper, or of the monk and the silver?"},
    };
    const Advice advice = f.director.advise("soul", "please do", Trust::Open, drew);
    CHECK(advice.item_id == "tale:1");
    CHECK(advice.note == "chose tale:1");
    CHECK(advice.direction.find("They have chosen this from what you offered") != std::string::npos);
}

TEST_CASE("'and the other one?' a turn later goes back to the offer, less what was told") {
    DirectorFixture f;
    with_tales(f.director);
    const std::vector<Drew> drew = {
        {"hub:tales", "Would you hear of the kidnapper, or of the monk and the silver?"},
        {"tale:1", "A kidnapper took the miller's child..."},
    };
    const Advice advice = f.director.advise("soul", "and the other one?", Trust::Open, drew);
    CHECK(advice.item_id == "tale:2");
    CHECK(advice.note == "chose tale:2");
}

TEST_CASE("'why?' after a fact asks for more of the same fact") {
    DirectorFixture f;
    with_tales(f.director);
    const std::vector<Drew> drew = {{"town:smith", "The smith's by the gate, as anyone could tell you."}};
    const Advice advice = f.director.advise("soul", "why?", Trust::Open, drew);
    CHECK(advice.item_id == "town:smith");
    CHECK(advice.note == "more on town:smith");
}

TEST_CASE("a short line after a reply that drew on nothing is honest ignorance") {
    DirectorFixture f;
    with_tales(f.director);
    const std::vector<Drew> drew = {{"", "Good day to you."}};
    const Advice advice = f.director.advise("soul", "why?", Trust::Open, drew);
    CHECK(advice.item_id.empty());
    CHECK(advice.note == "nothing above tau");
    // And a long line nothing is written for is not read as a follow-up at all.
    const std::vector<Drew> after_fact = {{"town:smith", "The smith's by the gate."}};
    const Advice longer = f.director.advise(
        "soul", "I was wondering whether you might know the way to the next village over the hill",
        Trust::Open, after_fact);
    CHECK(longer.note == "nothing above tau");
}

TEST_CASE("the embedder's say on an offer of help clears the threshold or the pattern does") {
    DirectorFixture f;
    float score = -1.0f;
    // Nothing the stub maps near the asks, so only the pattern can say yes.
    CHECK(f.director.is_quest_ask("What ails you?", &score));
    CHECK_FALSE(f.director.is_quest_ask("Where is the smith?", &score));
    // The asks were embedded once and kept.
    const int after_first = f.embedder.calls;
    f.director.is_quest_ask("Where is the smith?");
    CHECK(f.embedder.calls == after_first + 1);
}
