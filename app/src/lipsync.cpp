#include "lipsync.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace moonshine {

double LipPlan::seconds() const {
    double total = 0.0;
    for (const LipClip& clip : clips) total += clip.seconds;
    return total;
}

std::size_t LipPlan::rests() const {
    std::size_t count = 0;
    for (const LipClip& clip : clips) count += clip.rest();
    return count;
}

bool Facials::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;
    std::vector<Clip> clips, shut;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        Clip clip;
        if (!(fields >> clip.actor >> clip.stem >> clip.seconds)) continue;
        if (clip.seconds <= 0.0) continue;
        clip.talk = clip.seconds;
        clip.lead = 0.0;
        clip.still = 0.0;
        std::string mouth;
        if (fields >> mouth && !mouth.empty()) {
            // The digits: the first and last that are not 0 bound the speaking stretch, and
            // the zeros between them are the mouth shut mid-line.
            const std::size_t first = mouth.find_first_not_of('0');
            if (first == std::string::npos) {
                // A mouth that never opens speaks nothing, but it is what the face rests in.
                shut.push_back(std::move(clip));
                continue;
            }
            const std::size_t last = mouth.find_last_not_of('0');
            clip.lead = first * kMouthStep;
            clip.talk = std::min(clip.seconds, (last + 1) * kMouthStep);
            std::size_t shut = 0;
            for (std::size_t i = first; i <= last; ++i) shut += mouth[i] == '0';
            clip.still = static_cast<double>(shut) / static_cast<double>(last - first + 1);
        }
        if (clip.talk - clip.lead < 0.3) continue;
        clips.push_back(std::move(clip));
    }
    if (clips.empty()) return false;
    const auto by_actor_then_talk = [](const Clip& a, const Clip& b) {
        if (a.actor != b.actor) return a.actor < b.actor;
        if (a.talk != b.talk) return a.talk < b.talk;
        return a.stem < b.stem;
    };
    std::sort(clips.begin(), clips.end(), by_actor_then_talk);
    std::sort(shut.begin(), shut.end(), by_actor_then_talk);
    clips_ = std::move(clips);
    shut_ = std::move(shut);
    return true;
}

std::string Facials::shut(std::string_view actor) const {
    const auto nearest = [this](std::string_view who) -> const Clip* {
        const Clip* best = nullptr;
        for (const Clip& clip : shut_) {
            if (!who.empty() && clip.actor != who) continue;
            if (best == nullptr || std::fabs(clip.seconds - kShutSeconds) < std::fabs(best->seconds - kShutSeconds)) {
                best = &clip;
            }
        }
        return best;
    };
    const Clip* pick = nearest(actor);
    if (pick == nullptr) pick = nearest("");
    return pick == nullptr ? std::string() : pick->stem;
}

Facials::Range Facials::range(std::string_view actor) const {
    if (actor.empty()) return {0, clips_.size()};
    const auto first = std::lower_bound(clips_.begin(), clips_.end(), actor,
                                        [](const Clip& c, std::string_view a) { return c.actor < a; });
    const auto last = std::upper_bound(clips_.begin(), clips_.end(), actor,
                                       [](std::string_view a, const Clip& c) { return a < c.actor; });
    return {static_cast<std::size_t>(first - clips_.begin()), static_cast<std::size_t>(last - clips_.begin())};
}

std::size_t Facials::count(std::string_view actor) const {
    const Range r = range(actor);
    return r.last - r.first;
}

namespace {

// A small generator of its own, so a plan is the same for the same seed on every machine.
std::uint32_t next(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

}  // namespace

const Facials::Clip* Facials::pick(Range pool, double target, std::string_view not_this,
                                   std::uint32_t& rng) const {
    if (pool.first >= pool.last || target <= 0.0) return nullptr;
    // The clips nearest the target length, by the ratio of their speaking stretch to it,
    // the handful of them; one is drawn. A mouth shut mid-line or late to open costs as
    // much as being that far off in length: the player sees a pause either way.
    struct Near {
        double off;
        const Clip* clip;
    };
    std::vector<Near> near;
    near.reserve(16);
    constexpr std::size_t kChoices = 8;
    // From a seeded point in the pool round to it again, so that among the many clips of
    // one length in the whole table it is not always the first actor's that are nearest.
    const std::size_t count = pool.last - pool.first;
    const std::size_t from = next(rng) % count;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t i = pool.first + (from + k) % count;
        const Clip& clip = clips_[i];
        if (clip.stem == not_this) continue;
        const double off = std::fabs(std::log(clip.talk / target)) + clip.still + 2.0 * clip.lead;
        if (near.size() < kChoices) {
            near.push_back({off, &clip});
            if (near.size() == kChoices) {
                std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.off < b.off; });
            }
            continue;
        }
        if (off >= near.back().off) continue;
        near.back() = {off, &clip};
        std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.off < b.off; });
    }
    if (near.empty()) return nullptr;
    if (near.size() < kChoices) {
        std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.off < b.off; });
    }
    // Only the ones that fit without leaving the speed range, when any do.
    const double limit = std::log(kMaxSpeed);
    std::size_t fitting = 0;
    while (fitting < near.size() && near[fitting].off <= limit) ++fitting;
    const std::size_t among = fitting > 0 ? fitting : 1;
    return near[next(rng) % among].clip;
}

void Facials::fill(Range pool, double seconds, std::uint32_t& rng, std::string& last, LipPlan& plan) const {
    double remaining = seconds;
    std::size_t added = 0;
    while (remaining > kShortest && added < 16) {
        // A long line in pieces of about kPieceSeconds, cut evenly so the last is not a stub.
        double target = remaining;
        if (remaining > kPieceSeconds * 1.3) {
            const double pieces = std::ceil(remaining / kPieceSeconds);
            target = remaining / pieces;
        }
        const Clip* clip = pick(pool, target, last, rng);
        if (clip == nullptr) break;
        // The speaking stretch fitted to the share; the clip is cut where it ends, so the
        // closed mouth of its tail is never shown and the next clip takes over there -- or
        // where the speech ends, when even the slowest speed leaves it longer.
        double speed = clip->talk / target;
        speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
        const double covered = std::min(remaining, clip->talk / speed);
        plan.clips.push_back({clip->stem, speed, covered});
        remaining -= covered;
        last = clip->stem;
        ++added;
    }
}

LipPlan Facials::plan(std::string_view actor, double seconds, std::uint32_t seed) const {
    LipPlan plan;
    if (clips_.empty() || seconds < kShortest) return plan;
    plan.shut = shut(actor);
    Range pool = range(actor);
    if (pool.last - pool.first < kOwnFaceMinimum) pool = range("");
    std::uint32_t rng = seed * 2654435761u + 1u;
    std::string last;
    fill(pool, seconds, rng, last, plan);
    return plan;
}

Silences::Silences(int sample_rate)
    : window_(static_cast<std::size_t>(std::max(sample_rate, 50) * kWindowSeconds)) {}

void Silences::push(const std::int16_t* samples, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const double x = samples[i] / 32768.0;
        sum_squares_ += x * x;
        if (++filled_ < window_) continue;
        const double rms = std::sqrt(sum_squares_ / static_cast<double>(window_));
        const float db = static_cast<float>(20.0 * std::log10(rms + 1e-9));
        db_.push_back(db);
        peak_db_ = std::max(peak_db_, db);
        sum_squares_ = 0.0;
        filled_ = 0;
    }
}

void Silences::finish() {
    over_ = true;
}

Silences::Heard Silences::heard() const {
    Heard heard;
    heard.seconds = static_cast<double>(db_.size()) * kWindowSeconds;
    heard.over = over_;
    const float floor = static_cast<float>(std::clamp(static_cast<double>(peak_db_) - kDropDb, kFloorMinDb, kFloorMaxDb));
    const std::size_t least = static_cast<std::size_t>(std::ceil(kPauseSeconds / kWindowSeconds));
    std::size_t run = 0;
    for (std::size_t i = 0; i < db_.size(); ++i) {
        if (db_[i] < floor) {
            ++run;
            continue;
        }
        if (run >= least) heard.pauses.push_back({(i - run) * kWindowSeconds, i * kWindowSeconds});
        run = 0;
    }
    if (run > 0) heard.silent_since = (db_.size() - run) * kWindowSeconds;
    if (over_ && run >= least) heard.pauses.push_back({heard.silent_since, heard.seconds});
    return heard;
}

LipPlan Facials::plan(std::string_view actor, const Silences::Heard& heard, double from, double seconds,
                      std::uint32_t seed) const {
    LipPlan plan;
    plan.from = std::max(0.0, from);
    if (clips_.empty() || seconds - plan.from < kShortest) return plan;
    plan.shut = shut(actor);
    // The line as pieces of speech and rest, from its start: a rest at every pause heard,
    // speech between them, and after what was heard either nothing (the audio over) or
    // speech on to the end.
    struct Piece {
        double start, end;
        bool rest;
    };
    std::vector<Piece> pieces;
    double at = 0.0;
    const auto speech_to = [&](double end) {
        if (end > at) pieces.push_back({at, end, false});
        at = std::max(at, end);
    };
    const auto rest_to = [&](double end) {
        if (end > at) pieces.push_back({at, end, true});
        at = std::max(at, end);
    };
    for (const Pause& pause : heard.pauses) {
        speech_to(pause.start);
        rest_to(pause.end);
    }
    if (heard.over) {
        speech_to(heard.seconds);
    } else {
        speech_to(std::max(seconds, at));
    }
    // A clip after a rest starts a little before the word, so the blend is over by it.
    for (std::size_t i = 0; i + 1 < pieces.size(); ++i) {
        if (!pieces[i].rest || pieces[i].end - pieces[i].start <= 2 * kRestLead) continue;
        pieces[i].end -= kRestLead;
        pieces[i + 1].start -= kRestLead;
    }
    Range pool = range(actor);
    if (pool.last - pool.first < kOwnFaceMinimum) pool = range("");
    std::uint32_t rng = seed * 2654435761u + 1u;
    std::string last;
    const auto rest = [&plan](double length) {
        if (!plan.clips.empty() && plan.clips.back().rest()) {
            plan.clips.back().seconds += length;
        } else {
            plan.clips.push_back({"", 1.0, length});
        }
    };
    for (const Piece& piece : pieces) {
        if (piece.end <= plan.from) continue;
        const double start = std::max(piece.start, plan.from);
        const double length = piece.end - start;
        // A sliver of speech, which a revision's start can leave, is not worth a clip.
        if (piece.rest || length < kRestLead) {
            rest(length);
            continue;
        }
        // A word or two between pauses still moves the mouth: a short piece takes the
        // shortest clip there is, cut to it.
        const std::size_t before = plan.clips.size();
        fill(pool, std::max(length, kShortest + 1e-6), rng, last, plan);
        double covered = 0.0;
        for (std::size_t i = before; i < plan.clips.size(); ++i) covered += plan.clips[i].seconds;
        if (covered > length && !plan.clips.empty()) plan.clips.back().seconds -= covered - length;
    }
    // A plan that is nothing but rest says nothing the stop cue does not.
    if (plan.rests() == plan.clips.size()) plan.clips.clear();
    return plan;
}

std::uint32_t lip_key(std::string_view who, std::string_view text) {
    std::uint32_t h = 0;
    const auto take = [&h](unsigned char c) { h = (h * 31u + c) % kLipKeyModulus; };
    for (unsigned char c : who) take(c);
    take(' ');
    // Whitespace runs as one space, none at either end: the same words however they were
    // wrapped on the way here.
    bool spacing = false;
    bool begun = false;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            spacing = begun;
            continue;
        }
        if (spacing) take(' ');
        spacing = false;
        begun = true;
        take(c);
    }
    return h;
}

std::string lip_file(std::uint32_t key, int seq) {
    return "lip_" + std::to_string(key) + "_" + std::to_string(seq) + ".txt";
}

std::string lip_cue(std::string_view who, std::uint32_t key, const LipPlan& plan) {
    std::string body = "lip " + std::string(who) + " " + std::to_string(key) + "\n";
    char line[256];
    if (plan.from > 0.0) {
        std::snprintf(line, sizeof line, "from %.2f\n", plan.from);
        body += line;
    }
    if (!plan.shut.empty()) body += "shut " + plan.shut + "\n";
    for (const LipClip& clip : plan.clips) {
        if (clip.rest()) {
            std::snprintf(line, sizeof line, "rest %.2f\n", clip.seconds);
        } else {
            std::snprintf(line, sizeof line, "play %s %.2f %.2f\n", clip.clip.c_str(), clip.speed, clip.seconds);
        }
        body += line;
    }
    return body;
}

std::string lip_mouth(std::string_view who, std::uint32_t key, const std::vector<Mouth>& mouth) {
    std::string body = "lip " + std::string(who) + " " + std::to_string(key) + "\n";
    char line[128];
    for (const Mouth& m : mouth) {
        std::snprintf(line, sizeof line, "mouth %.3f %s\n", m.at, m.viseme.c_str());
        body += line;
    }
    return body;
}

std::string lip_stop(std::string_view who, std::uint32_t key) {
    return "lip " + std::string(who) + " " + std::to_string(key) + "\nstop\n";
}

}  // namespace moonshine
