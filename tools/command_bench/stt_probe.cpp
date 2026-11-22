// C++20 Spoken-Utterance Synthesizer + Moonshine `medium-streaming-en` Two-Pass STT Benchmark.
//
// Uses `PhononEngine` (`app/src/phonon.h`) to synthesize 500 spoken test utterances across
// 10 voices, adds realistic room reflection + microphone noise at 16 kHz, and transcribes
// each buffered utterance through `moonshine::Transcriber` (`medium-streaming-en`) under:
//   1. Baseline `moonshine_transcriber_set_context` (current `Session::listen_context()`)
//   2. Pass-1 proactive Hop-1 circle/kin `moonshine_transcriber_set_keyterms`
//   3. Pass-2 expanded Hop-1 + Hop-2 `moonshine_transcriber_set_keyterms` (buffered PCM replay)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "json.hpp"
#include "listen.h"
#include "moonshine-c-api.h"
#include "moonshine-cpp.h"
#include "phonon.h"

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::vector<float> resample_and_perturb_to_16k(
    const std::vector<std::int16_t>& src, int src_rate, std::uint32_t seed) {
    if (src.empty() || src_rate <= 0) return {};
    constexpr int dst_rate = 16000;
    const double ratio = static_cast<double>(src_rate) / static_cast<double>(dst_rate);
    const std::size_t out_len =
        static_cast<std::size_t>(static_cast<double>(src.size()) / ratio);

    std::vector<float> pcm(out_len, 0.0f);
    for (std::size_t i = 0; i < out_len; ++i) {
        const double pos = static_cast<double>(i) * ratio;
        const std::size_t idx = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(idx);
        const float a = src[std::min(idx, src.size() - 1)] / 32768.0f;
        const float b = src[std::min(idx + 1, src.size() - 1)] / 32768.0f;
        pcm[i] = static_cast<float>(a * (1.0 - frac) + b * frac);
    }

    // Add 0.15s leading pad and 0.65s trailing pad so streaming VAD cleanly closes final segment
    std::vector<float> padded(2400 + pcm.size() + 10400, 0.0f);
    std::copy(pcm.begin(), pcm.end(), padded.begin() + 2400);

    // Subtle 2-tap room reflection (18 ms and 37 ms) + ~23 dB SNR desktop mic noise
    std::vector<float> room = padded;
    for (std::size_t i = 0; i < padded.size(); ++i) {
        float v = padded[i];
        if (i >= 288) v += 0.14f * padded[i - 288];
        if (i >= 592) v += 0.07f * padded[i - 592];
        room[i] = v;
    }

    double sum_sq = 0.0;
    for (float x : pcm) sum_sq += static_cast<double>(x) * static_cast<double>(x);
    const double rms = std::sqrt(sum_sq / std::max<std::size_t>(1, pcm.size()));
    const float noise_std = static_cast<float>(rms * 0.07);  // ~23 dB SNR

    std::mt19937 rng(seed);
    std::normal_distribution<float> norm(0.0f, noise_std > 1e-5f ? noise_std : 0.001f);
    for (float& x : room) {
        x = std::clamp(x + norm(rng), -0.99f, 0.99f);
    }
    return room;
}

std::string flatten_transcript(const ::moonshine::Transcript& t) {
    std::string out;
    for (const auto& line : t.lines) {
        std::string_view p = line.text;
        while (!p.empty() && std::isspace(static_cast<unsigned char>(p.front()))) p.remove_prefix(1);
        while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back()))) p.remove_suffix(1);
        if (p.empty()) continue;
        if (!out.empty()) out += ' ';
        out.append(p.data(), p.size());
    }
    return out;
}

std::pair<std::string, double> transcribe_pcm_streaming(
    ::moonshine::Transcriber& tr, const std::vector<float>& pcm16k) {
    constexpr std::size_t chunk_samples = 1600;  // 100 ms chunks, matching `Ear::kChunkFrames`
    const auto t0 = Clock::now();
    auto stream = tr.createStream(1.0e9, 0);
    stream.start();
    std::size_t fed = 0;
    std::size_t since_upd = 0;
    while (fed < pcm16k.size()) {
        const std::size_t n = std::min(chunk_samples, pcm16k.size() - fed);
        std::vector<float> piece(pcm16k.begin() + static_cast<std::ptrdiff_t>(fed),
                                 pcm16k.begin() + static_cast<std::ptrdiff_t>(fed + n));
        stream.addAudio(piece, 16000);
        fed += n;
        since_upd += n;
        if (since_upd >= 6400) {  // every 400 ms, matching `Ear::kReadEvery`
            since_upd = 0;
            (void)stream.updateTranscription(0);
        }
    }
    stream.stop();
    const auto final_t = stream.updateTranscription(0);
    std::string text = flatten_transcript(final_t);
    stream.close();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return {text, ms};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: moonshine_stt_probe <stt_manifest.jsonl> <stt_raw_out.jsonl> [stt_model_dir]\n";
        return 1;
    }
    const std::string manifest_path = argv[1];
    const std::string out_path = argv[2];
    const std::string stt_dir =
        (argc >= 4) ? argv[3] : moonshine::find_stt_model("models/stt/medium-streaming-en").string();

    const int arch = moonshine::stt_arch_for(stt_dir);
    if (arch < 0) {
        std::cerr << "Invalid STT model dir: " << stt_dir << "\n";
        return 2;
    }

    moonshine::PhononEngine tts(moonshine::PhononEngine::find_model(), {});
    if (!tts.ready()) {
        std::cerr << "Phonon not ready: " << tts.failure() << "\n";
        return 2;
    }

    ::moonshine::Transcriber tr(
        stt_dir,
        static_cast<::moonshine::ModelArch>(arch),
        1.0e9,
        "",
        {{"keyterm_boost", std::to_string(moonshine::kContextBoost)},
         {"context_max_terms", std::to_string(moonshine::kContextMaxTerms)}});
    const int32_t handle = tr.getHandle();

    std::ifstream in(manifest_path);
    std::ofstream out(out_path);
    std::string line;
    int idx = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json row = json::parse(line);
        const std::string eid = row.value("example_id", "");
        const std::string voice = row.value("voice", "toby");
        const std::string text = row.value("text", "");
        const std::string base_ctx = row.value("baseline_context", "");
        const std::string p1_terms = row.value("pass1_keyterms", "");
        const std::string p2_terms = row.value("pass2_keyterms", "");

        auto synth = tts.speak(voice, text);
        std::vector<float> pcm16k = resample_and_perturb_to_16k(
            synth.samples, synth.rate, static_cast<std::uint32_t>(1000 + idx));
        const double audio_sec = static_cast<double>(pcm16k.size()) / 16000.0;

        // 1. Baseline `set_context` (current shipping behavior)
        moonshine_transcriber_set_keyterms(handle, "");
        moonshine_transcriber_set_context(handle, base_ctx.c_str(), moonshine::kContextMaxTerms);
        auto [tx_base, ms_base] = transcribe_pcm_streaming(tr, pcm16k);

        // 2. Pass-1 proactive Hop-1 `set_keyterms`
        moonshine_transcriber_set_context(handle, "", 0);
        moonshine_transcriber_set_keyterms(handle, p1_terms.c_str());
        auto [tx_p1, ms_p1] = transcribe_pcm_streaming(tr, pcm16k);

        // 3. Pass-2 expanded Hop-1 + Hop-2 `set_keyterms` (replay of buffered PCM)
        moonshine_transcriber_set_keyterms(handle, p2_terms.c_str());
        auto [tx_p2, ms_p2] = transcribe_pcm_streaming(tr, pcm16k);

        json res = {
            {"example_id", eid},
            {"gold_text", text},
            {"audio_sec", audio_sec},
            {"tx_baseline", tx_base},
            {"ms_baseline", ms_base},
            {"tx_pass1", tx_p1},
            {"ms_pass1", ms_p1},
            {"tx_pass2", tx_p2},
            {"ms_pass2", ms_p2},
        };
        out << res.dump() << "\n";
        ++idx;
        if (idx % 50 == 0) {
            std::cerr << "[STT Probe] " << idx << " audio_sec=" << audio_sec
                      << " base='" << tx_base << "' | p1='" << tx_p1 << "' | p2='" << tx_p2 << "'\n";
        }
    }
    return 0;
}
