#include "doctest.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#include "lipsync.h"

using namespace moonshine;

namespace {

// An actor with 56 clips from 1 to 12 seconds (enough for a face of their own), and
// another with only two.
std::filesystem::path write_index() {
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "moonshine-facials-test.txt";
    std::ofstream out(file);
    out << "# actor stem seconds\n";
    for (int i = 0; i < 56; ++i) {
        const double seconds = 1.0 + i * 0.2;
        out << "pdea pdea_a_" << i << " " << seconds << "\n";
    }
    out << "mwol mwol_b_3 3.00\n"
           "mwol mwol_b_5 5.00\n";
    return file;
}

}  // namespace

TEST_CASE("the key is the same sum the mod makes, over who and the flattened words") {
    // h = h * 31 + byte, mod 2^19 - 1, over "npc Good day." -- and every step's product
    // under 2^24, since the game's Lua does this in 32-bit floats (lipsync.h).
    std::uint32_t h = 0;
    for (unsigned char c : std::string("npc Good day.")) {
        CHECK(h * 31u + c < (1u << 24));
        h = (h * 31u + c) % kLipKeyModulus;
    }
    CHECK(h < kLipKeyModulus);
    CHECK(lip_key("npc", "Good day.") == h);
    CHECK(lip_key("npc", "  Good\n day.  ") == h);
    CHECK(lip_key("npc", "Good    day.") == h);
    CHECK(lip_key("henry", "Good day.") != h);
    CHECK(lip_file(h, 1) == "lip_" + std::to_string(h) + "_1.txt");
}

TEST_CASE("a line of about a clip's length gets that clip at the speed that fits") {
    Facials facials;
    REQUIRE(facials.load(write_index()));
    CHECK(facials.size() == 58);
    CHECK(facials.count("pdea") == 56);
    // Fewer than the own-face minimum, so anybody's clips are used for mwol.
    CHECK(facials.count("mwol") == 2);

    const LipPlan plan = facials.plan("pdea", 4.4, 7);
    REQUIRE(plan.clips.size() == 1);
    CHECK(plan.clips[0].clip.rfind("pdea_", 0) == 0);
    CHECK(plan.clips[0].speed >= Facials::kMinSpeed);
    CHECK(plan.clips[0].speed <= Facials::kMaxSpeed);
    CHECK(plan.clips[0].seconds == doctest::Approx(4.4).epsilon(0.01));
    CHECK(plan.seconds() == doctest::Approx(4.4).epsilon(0.01));
}

TEST_CASE("a long line is cut into pieces that follow each other, no clip twice running") {
    Facials facials;
    REQUIRE(facials.load(write_index()));
    const LipPlan plan = facials.plan("pdea", 25.0, 3);
    REQUIRE(plan.clips.size() >= 3);
    CHECK(plan.seconds() == doctest::Approx(25.0).epsilon(0.05));
    for (std::size_t i = 1; i < plan.clips.size(); ++i) {
        CHECK(plan.clips[i].clip != plan.clips[i - 1].clip);
    }
    for (const LipClip& clip : plan.clips) {
        CHECK(clip.speed >= Facials::kMinSpeed);
        CHECK(clip.speed <= Facials::kMaxSpeed);
    }
}

TEST_CASE("the seed picks among the near lengths, so the same words do not always wear one face") {
    Facials facials;
    REQUIRE(facials.load(write_index()));
    std::set<std::string> seen;
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        const LipPlan plan = facials.plan("pdea", 6.0, seed);
        REQUIRE(plan.clips.size() == 1);
        seen.insert(plan.clips[0].clip);
    }
    CHECK(seen.size() > 1);
    // But the same seed is the same plan, on any machine.
    CHECK(facials.plan("pdea", 6.0, 5).clips[0].clip == facials.plan("pdea", 6.0, 5).clips[0].clip);
}

TEST_CASE("an actor with too few clips borrows anybody's; nothing loaded is no plan") {
    Facials facials;
    REQUIRE(facials.load(write_index()));
    const LipPlan borrowed = facials.plan("mwol", 9.0, 1);
    REQUIRE(borrowed.clips.size() == 1);
    CHECK(borrowed.clips[0].clip.rfind("pdea_", 0) == 0);
    CHECK(facials.plan("pdea", 0.1, 1).empty());

    Facials none;
    CHECK_FALSE(none.load(std::filesystem::temp_directory_path() / "no-such-facials.txt"));
    CHECK(none.plan("pdea", 4.0, 1).empty());
}

TEST_CASE("a clip is fitted by the stretch of it that speaks and cut where its mouth closes") {
    // Two 4-second clips: one speaks for all of it, one for 2.5 s and then holds its mouth
    // shut (the usual shape of a shipped clip), and one that speaks 4 s but keeps shutting
    // mid-line. The mouth column is a digit per 100 ms.
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "moonshine-facials-mouth-test.txt";
    {
        std::ofstream out(file);
        out << "# actor stem seconds mouth\n"
               "jnun jnun_all 4.00 3454543545354345454345354534543543\n"
               "jnun jnun_tail 4.00 3454543545354345454345354000000000000000\n"
               "jnun jnun_gappy 4.00 3400004500000034000045000000300004\n"
               "jnun jnun_mute 4.00 0000000000000000000000000000000000000000\n"
               "jnun jnun_hum 1.20 000000000000\n"
               "abcd abcd_hum 0.90 000000000\n"
               "jnun jnun_old 4.00\n";
    }
    Facials facials;
    REQUIRE(facials.load(file));
    CHECK(facials.size() == 4);  // the mute ones speak nothing, so they are not fitted to a line
    // ...but they are what the face rests in: the actor's own nearest a second long, anybody's
    // for an actor with none, and every plan names it.
    CHECK(facials.shut("jnun") == "jnun_hum");
    CHECK(facials.shut("abcd") == "abcd_hum");
    CHECK(facials.shut("zzzz") == "abcd_hum");
    CHECK(facials.plan("jnun", 2.5, 1).shut == "jnun_hum");

    // A 2.5 s line: the clip with the 2.5 s speaking stretch fits at speed 1 and is cut
    // there, not stretched to its 4 s file length.
    bool tail_fitted = false;
    for (std::uint32_t seed = 0; seed < 20 && !tail_fitted; ++seed) {
        const LipPlan plan = facials.plan("jnun", 2.5, seed);
        REQUIRE(plan.clips.size() == 1);
        if (plan.clips[0].clip == "jnun_tail") {
            CHECK(plan.clips[0].speed == doctest::Approx(1.0).epsilon(0.05));
            CHECK(plan.clips[0].seconds == doctest::Approx(2.5).epsilon(0.05));
            tail_fitted = true;
        }
    }
    CHECK(tail_fitted);

    // A 4 s line: the clip that keeps moving is preferred to the gappy one of the same
    // length, every seed.
    for (std::uint32_t seed = 0; seed < 20; ++seed) {
        const LipPlan plan = facials.plan("jnun", 4.0, seed);
        REQUIRE(plan.clips.size() == 1);
        CHECK(plan.clips[0].clip != "jnun_gappy");
    }
}

TEST_CASE("the cue files read as the mod parses them") {
    LipPlan plan;
    plan.clips.push_back({"pdea_a_4", 1.25, 3.2});
    plan.clips.push_back({"pdea_a_7", 0.9, 7.78});
    CHECK(lip_cue("npc", 42u, plan) ==
          "lip npc 42\n"
          "play pdea_a_4 1.25 3.20\n"
          "play pdea_a_7 0.90 7.78\n");
    CHECK(lip_stop("henry", 42u) == "lip henry 42\nstop\n");
    // A revision: from its second, a rest where the speech pauses, then the clips.
    LipPlan revised;
    revised.from = 2.3;
    revised.clips.push_back({"", 1.0, 0.4});
    revised.clips.push_back({"pdea_a_2", 1.1, 1.5});
    CHECK(revised.rests() == 1);
    CHECK(lip_cue("npc", 42u, revised) ==
          "lip npc 42\n"
          "from 2.30\n"
          "rest 0.40\n"
          "play pdea_a_2 1.10 1.50\n");
    // With a shut clip for the face: named under the header, before the plan.
    revised.shut = "pdea_hum";
    CHECK(lip_cue("npc", 42u, revised) ==
          "lip npc 42\n"
          "from 2.30\n"
          "shut pdea_hum\n"
          "rest 0.40\n"
          "play pdea_a_2 1.10 1.50\n");
}

namespace {

// A line's audio: a tone through the stretches that speak, nothing through the rest,
// 24 kHz, as a Heard, pushed in chunks of `chunk` seconds (the whole at once for 0).
Silences::Heard heard_of(std::initializer_list<std::pair<double, double>> speech, double seconds, bool over,
                         double chunk = 0.0) {
    constexpr int rate = 24000;
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(seconds * rate), 0);
    for (const auto& [start, end] : speech) {
        for (std::size_t i = static_cast<std::size_t>(start * rate); i < static_cast<std::size_t>(end * rate) && i < pcm.size(); ++i) {
            pcm[i] = static_cast<std::int16_t>(8000.0 * std::sin(i * 0.05));
        }
    }
    Silences silences(rate);
    const std::size_t step = chunk > 0 ? static_cast<std::size_t>(chunk * rate) : pcm.size();
    for (std::size_t at = 0; at < pcm.size(); at += step) {
        silences.push(pcm.data() + at, std::min(step, pcm.size() - at));
    }
    if (over) silences.finish();
    return silences.heard();
}

}  // namespace

TEST_CASE("the silences of the audio are its pauses, once sound has followed them or it has ended") {
    // Speech 0.5-2.0, a 0.4 s pause, speech 2.4-3.0, a breath of 0.1 s (no pause), speech
    // 3.1-4.0, then quiet to the end at 4.6.
    const Silences::Heard all = heard_of({{0.5, 2.0}, {2.4, 3.0}, {3.1, 4.0}}, 4.6, true);
    CHECK(all.over);
    CHECK(all.seconds == doctest::Approx(4.6).epsilon(0.01));
    REQUIRE(all.pauses.size() == 3);
    CHECK(all.pauses[0].start == doctest::Approx(0.0).epsilon(0.01));
    CHECK(all.pauses[0].end == doctest::Approx(0.5).epsilon(0.05));
    CHECK(all.pauses[1].start == doctest::Approx(2.0).epsilon(0.02));
    CHECK(all.pauses[1].end == doctest::Approx(2.4).epsilon(0.02));
    CHECK(all.pauses[2].start == doctest::Approx(4.0).epsilon(0.02));
    CHECK(all.pauses[2].end == doctest::Approx(4.6).epsilon(0.01));
    // The same heard to 2.2 s, in chunks: the lead is a pause, the silence since 2.0 is
    // still open, and the audio is not over.
    const Silences::Heard part = heard_of({{0.5, 2.0}}, 2.2, false, 0.8);
    CHECK_FALSE(part.over);
    REQUIRE(part.pauses.size() == 1);
    CHECK(part.pauses[0].end == doctest::Approx(0.5).epsilon(0.05));
    CHECK(part.silent_since == doctest::Approx(2.0).epsilon(0.02));
    // Ending in sound: no open silence.
    CHECK(heard_of({{0.0, 1.0}}, 1.0, false).silent_since < 0.0);
    // A silence shorter than a pause ending the audio is not one.
    const Silences::Heard tail = heard_of({{0.0, 1.0}}, 1.1, true);
    CHECK(tail.pauses.empty());
    CHECK(tail.silent_since == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("the pauses heard cut the plan, a rest in each, the clips between them") {
    Facials facials;
    REQUIRE(facials.load(write_index()));
    // Speech to 1.0, a pause of 0.5, speech through a breath to 3.9, quiet to 4.5.
    const Silences::Heard all = heard_of({{0.0, 1.0}, {1.5, 2.6}, {2.7, 3.9}}, 4.5, true);
    const LipPlan plan = facials.plan("pdea", all, 0.0, 4.5, 7);
    REQUIRE(plan.clips.size() == 4);
    CHECK(plan.from == 0.0);
    CHECK_FALSE(plan.clips[0].rest());
    CHECK(plan.clips[0].seconds == doctest::Approx(1.0).epsilon(0.03));
    // The rest from 1.0 to 1.5, shortened by the lead so the clip after it is in by 1.5.
    CHECK(plan.clips[1].rest());
    CHECK(plan.clips[1].seconds == doctest::Approx(0.5 - Facials::kRestLead).epsilon(0.05));
    CHECK_FALSE(plan.clips[2].rest());
    CHECK(plan.clips[2].seconds == doctest::Approx(2.4 + Facials::kRestLead).epsilon(0.03));
    // And the audio's tail, the audio being over.
    CHECK(plan.clips[3].rest());
    CHECK(plan.clips[3].seconds == doctest::Approx(0.6).epsilon(0.05));
    CHECK(plan.seconds() == doctest::Approx(4.5).epsilon(0.01));
    CHECK(plan.rests() == 2);
    CHECK(plan.clips[0].clip != plan.clips[2].clip);

    // Heard only to 2.2 s with more to come: the pause, then speech taken to go on to the
    // guessed length.
    const Silences::Heard part = heard_of({{0.0, 1.0}, {1.5, 2.2}}, 2.2, false);
    const LipPlan open = facials.plan("pdea", part, 0.0, 6.0, 7);
    REQUIRE(open.clips.size() == 3);
    CHECK(open.clips[1].rest());
    CHECK_FALSE(open.clips.back().rest());
    CHECK(open.seconds() == doctest::Approx(6.0).epsilon(0.01));

    // A voice slow to begin: a rest first.
    const LipPlan late = facials.plan("pdea", heard_of({{0.5, 2.0}}, 2.0, false), 0.0, 3.0, 7);
    REQUIRE(late.clips.size() == 2);
    CHECK(late.clips[0].rest());
    CHECK(late.clips[0].seconds == doctest::Approx(0.4).epsilon(0.05));
    CHECK(late.clips[1].seconds == doctest::Approx(2.6).epsilon(0.03));

    // Nothing heard yet is the plain plan.
    const LipPlan plain = facials.plan("pdea", Silences::Heard{}, 0.0, 4.4, 7);
    REQUIRE(plain.clips.size() == 1);
    CHECK(plain.clips[0].clip == facials.plan("pdea", 4.4, 7).clips[0].clip);
    CHECK(plain.rests() == 0);
}

TEST_CASE("a revision starts at its second, with what came before left out") {
    Facials facials;
    REQUIRE(facials.load(write_index()));
    // Speech 0-1, pause to 1.5, speech to 2.5, pause to 3.0, speech on past 3.6.
    const Silences::Heard part = heard_of({{0.0, 1.0}, {1.5, 2.5}, {3.0, 3.6}}, 3.6, false);
    const LipPlan plan = facials.plan("pdea", part, 1.0, 6.0, 11);
    REQUIRE(plan.clips.size() == 4);
    CHECK(plan.from == 1.0);
    CHECK(plan.clips[0].rest());
    CHECK(plan.clips[0].seconds == doctest::Approx(0.4).epsilon(0.05));
    CHECK(plan.clips[1].seconds == doctest::Approx(1.1).epsilon(0.03));
    CHECK(plan.clips[2].rest());
    CHECK(plan.clips[2].seconds == doctest::Approx(0.4).epsilon(0.05));
    CHECK(plan.clips[3].seconds == doctest::Approx(3.1).epsilon(0.03));
    CHECK(plan.seconds() == doctest::Approx(5.0).epsilon(0.01));
    // From the second pause, the audio over at 3.6: a rest and the last stretch's clip,
    // cut to it.
    const Silences::Heard all = heard_of({{0.0, 1.0}, {1.5, 2.5}, {3.0, 3.6}}, 3.6, true);
    const LipPlan tail = facials.plan("pdea", all, 2.5, 3.6, 11);
    REQUIRE(tail.clips.size() == 2);
    CHECK(tail.clips[0].rest());
    CHECK(tail.clips[1].seconds == doctest::Approx(0.7).epsilon(0.03));
    // A plan of nothing but rest is no plan; past the end is none either.
    const Silences::Heard quiet = heard_of({{0.0, 1.0}}, 2.0, true);
    CHECK(facials.plan("pdea", quiet, 1.0, 2.0, 11).empty());
    CHECK(facials.plan("pdea", all, 5.0, 4.5, 11).empty());
}
