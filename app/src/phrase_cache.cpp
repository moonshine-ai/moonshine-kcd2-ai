#include "phrase_cache.h"

#include <opus.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace moonshine {
namespace {

// SHA-256, so a file name is the words and not a hash that two lines could share.
// The one test vector ("abc") is in test_phrase_cache.cpp.
void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    auto rotr = [](std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); };
    std::uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    std::uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    std::size_t padded = len + 1 + 8;
    if (padded % 64) padded += 64 - padded % 64;
    std::vector<std::uint8_t> block(padded, 0);
    if (len) std::memcpy(block.data(), data, len);
    block[len] = 0x80;
    for (int i = 0; i < 8; ++i) block[padded - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));

    for (std::size_t off = 0; off < padded; off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(block[off + 4 * i]) << 24) |
                   (std::uint32_t(block[off + 4 * i + 1]) << 16) |
                   (std::uint32_t(block[off + 4 * i + 2]) << 8) |
                   std::uint32_t(block[off + 4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }
    const std::uint32_t hs[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(hs[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(hs[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(hs[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(hs[i]);
    }
}

std::string hex32(const std::uint8_t dig[32]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = kHex[dig[i] >> 4];
        out[2 * i + 1] = kHex[dig[i] & 0xf];
    }
    return out;
}

// 20 ms, a frame Opus accepts at every rate it encodes.
int frame_for(int rate) {
    switch (rate) {
        case 8000:
            return 160;
        case 12000:
            return 240;
        case 16000:
            return 320;
        case 24000:
            return 480;
        case 48000:
            return 960;
        default:
            return 0;
    }
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
}

class Reader {
public:
    Reader(const std::uint8_t* bytes, std::size_t size) : p_(bytes), n_(size) {}
    bool take(void* dest, std::size_t n) {
        if (n > n_) return false;
        std::memcpy(dest, p_, n);
        p_ += n;
        n_ -= n;
        return true;
    }
    bool u16(std::uint16_t& value) {
        std::uint8_t b[2];
        if (!take(b, 2)) return false;
        value = static_cast<std::uint16_t>(b[0] | (b[1] << 8));
        return true;
    }
    bool u32(std::uint32_t& value) {
        std::uint8_t b[4];
        if (!take(b, 4)) return false;
        value = std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) |
                (std::uint32_t(b[3]) << 24);
        return true;
    }
    const std::uint8_t* data() const { return p_; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
};

struct EncDel {
    void operator()(OpusEncoder* p) const { opus_encoder_destroy(p); }
};
struct DecDel {
    void operator()(OpusDecoder* p) const { opus_decoder_destroy(p); }
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

}  // namespace

double phrase_compression(std::size_t bytes, std::size_t samples, int rate) {
    if (bytes == 0 || samples == 0 || rate <= 0) return 0;
    const double seconds = static_cast<double>(samples) / static_cast<double>(rate);
    const double pcm = seconds * static_cast<double>(kPhrasePcmRate) * 2.0;
    return pcm / static_cast<double>(bytes);
}

std::string phrase_flat(std::string_view text) {
    std::string flat;
    flat.reserve(text.size());
    bool spacing = false;
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            spacing = !flat.empty();
            continue;
        }
        if (spacing) flat.push_back(' ');
        spacing = false;
        flat.push_back(c);
    }
    return flat;
}

std::string phrase_key(std::string_view actor, std::string_view text) {
    const std::string flat = phrase_flat(text);
    std::string material;
    material.reserve(actor.size() + 1 + flat.size());
    material.append(actor);
    material.push_back('\n');
    material.append(flat);
    std::uint8_t dig[32];
    sha256(reinterpret_cast<const std::uint8_t*>(material.data()), material.size(), dig);
    return hex32(dig);
}

std::vector<std::uint8_t> encode_opus(const std::int16_t* pcm, std::size_t count, int rate) {
    if (pcm == nullptr || count == 0 || rate <= 0) {
        throw std::runtime_error("phrase cache: nothing to encode");
    }
    const int frame = frame_for(rate);
    if (frame == 0) {
        throw std::runtime_error("phrase cache: Opus has no 20 ms frame at " + std::to_string(rate) +
                                 " Hz");
    }
    int err = 0;
    std::unique_ptr<OpusEncoder, EncDel> enc(
        opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &err));
    if (!enc || err != OPUS_OK) {
        throw std::runtime_error(std::string("phrase cache: ") + opus_strerror(err));
    }
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(kPhraseBitrate));
    opus_encoder_ctl(enc.get(), OPUS_SET_VBR(0));
    opus_encoder_ctl(enc.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc.get(), OPUS_GET_LOOKAHEAD(&lookahead));
    if (lookahead < 0) lookahead = 0;

    // The decoder delays the signal by `lookahead` samples, so the input is padded
    // by at least that, up to a whole frame, and the count of real samples is kept
    // in the header. What is decoded is cut back to it.
    const std::size_t need = count + static_cast<std::size_t>(lookahead);
    const std::size_t frames = (need + static_cast<std::size_t>(frame) - 1) / static_cast<std::size_t>(frame);
    std::vector<std::int16_t> padded(frames * static_cast<std::size_t>(frame), 0);
    std::memcpy(padded.data(), pcm, count * sizeof(std::int16_t));

    std::vector<std::uint8_t> packets;
    std::vector<std::uint16_t> lengths;
    std::vector<unsigned char> packet(4000);
    for (std::size_t f = 0; f < frames; ++f) {
        const int n = opus_encode(enc.get(), padded.data() + f * static_cast<std::size_t>(frame), frame,
                                  packet.data(), static_cast<opus_int32>(packet.size()));
        if (n < 0) throw std::runtime_error(std::string("phrase cache: ") + opus_strerror(n));
        lengths.push_back(static_cast<std::uint16_t>(n));
        packets.insert(packets.end(), packet.begin(), packet.begin() + n);
    }

    std::vector<std::uint8_t> out;
    out.reserve(24 + packets.size() + lengths.size() * 2);
    out.push_back('M');
    out.push_back('S');
    out.push_back('O');
    out.push_back('P');
    put16(out, 1);
    put32(out, static_cast<std::uint32_t>(rate));
    put32(out, static_cast<std::uint32_t>(count));
    put16(out, static_cast<std::uint16_t>(frame));
    put16(out, static_cast<std::uint16_t>(lookahead));
    put16(out, static_cast<std::uint16_t>(kPhraseBitrate / 1000));
    put32(out, static_cast<std::uint32_t>(lengths.size()));
    std::size_t at = 0;
    for (std::uint16_t n : lengths) {
        put16(out, n);
        out.insert(out.end(), packets.begin() + static_cast<std::ptrdiff_t>(at),
                   packets.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    return out;
}

PhraseAudio decode_opus(const std::uint8_t* bytes, std::size_t size) {
    Reader in(bytes, size);
    char magic[4];
    if (!in.take(magic, 4) || std::memcmp(magic, "MSOP", 4) != 0) {
        throw std::runtime_error("phrase cache: not a cache file");
    }
    std::uint16_t version = 0, frame = 0, lookahead = 0, bitrate_kbps = 0;
    std::uint32_t rate = 0, count = 0, packets = 0;
    if (!in.u16(version) || version != 1 || !in.u32(rate) || !in.u32(count) || !in.u16(frame) ||
        !in.u16(lookahead) || !in.u16(bitrate_kbps) || !in.u32(packets)) {
        throw std::runtime_error("phrase cache: truncated header");
    }
    (void)bitrate_kbps;
    if (rate == 0 || frame == 0 || count == 0 || packets == 0 || packets > 100000) {
        throw std::runtime_error("phrase cache: header out of range");
    }
    int err = 0;
    std::unique_ptr<OpusDecoder, DecDel> dec(opus_decoder_create(static_cast<int>(rate), 1, &err));
    if (!dec || err != OPUS_OK) {
        throw std::runtime_error(std::string("phrase cache: ") + opus_strerror(err));
    }
    std::vector<std::int16_t> decoded;
    decoded.reserve(static_cast<std::size_t>(count) + lookahead + frame);
    std::vector<std::int16_t> frame_pcm(frame);
    for (std::uint32_t i = 0; i < packets; ++i) {
        std::uint16_t n = 0;
        if (!in.u16(n) || n == 0) throw std::runtime_error("phrase cache: truncated packet");
        std::vector<std::uint8_t> held(n);
        if (!in.take(held.data(), n)) throw std::runtime_error("phrase cache: truncated packet");
        const int got = opus_decode(dec.get(), held.data(), n, frame_pcm.data(), frame, 0);
        if (got < 0) throw std::runtime_error(std::string("phrase cache: ") + opus_strerror(got));
        decoded.insert(decoded.end(), frame_pcm.begin(), frame_pcm.begin() + got);
    }
    if (decoded.size() < static_cast<std::size_t>(lookahead) + count) {
        throw std::runtime_error("phrase cache: decoded short of the line");
    }
    PhraseAudio audio;
    audio.rate = static_cast<int>(rate);
    audio.pcm.assign(decoded.begin() + lookahead, decoded.begin() + lookahead + count);
    return audio;
}

PhraseCache::PhraseCache(std::filesystem::path dir) { open(std::move(dir)); }

bool PhraseCache::open(const std::filesystem::path& dir) {
    dir_ = dir;
    count_ = 0;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (entry.path().extension() == ".opus") ++count_;
    }
    return !error;
}

std::optional<PhraseAudio> PhraseCache::find(std::string_view actor, std::string_view text) const {
    if (dir_.empty() || phrase_flat(text).empty()) return std::nullopt;
    const std::filesystem::path path = dir_ / (phrase_key(actor, text) + ".opus");
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
    const std::vector<std::uint8_t> bytes = read_file(path);
    if (bytes.empty()) return std::nullopt;
    try {
        return decode_opus(bytes.data(), bytes.size());
    } catch (const std::exception& why) {
        std::cerr << "voice: " << why.what() << " (" << path.filename().string() << ")\n";
        return std::nullopt;
    }
}

StoredPhrase PhraseCache::store(std::string_view actor, std::string_view text, const std::int16_t* pcm,
                                std::size_t count, int rate) {
    const std::string flat = phrase_flat(text);
    if (flat.empty()) throw std::runtime_error("phrase cache: nothing to store");
    const std::vector<std::uint8_t> bytes = encode_opus(pcm, count, rate);
    if (dir_.empty()) throw std::runtime_error("phrase cache: no directory");
    std::filesystem::create_directories(dir_);
    const std::string key = phrase_key(actor, flat);
    const std::filesystem::path path = dir_ / (key + ".opus");
    const bool replacing = std::filesystem::is_regular_file(path);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("phrase cache: cannot write " + path.string());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("phrase cache: cannot write " + path.string());
    }
    if (!replacing) ++count_;

    const std::filesystem::path index = dir_ / "index.tsv";
    std::vector<std::string> lines;
    {
        std::ifstream in(index);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind(key, 0) == 0 && line.size() > key.size() && line[key.size()] == '\t') continue;
            lines.push_back(std::move(line));
        }
    }
    lines.push_back(key + "\t" + std::string(actor) + "\t" + flat);
    std::ofstream out(index, std::ios::trunc);
    if (!out) throw std::runtime_error("phrase cache: cannot write " + index.string());
    out << "# hash, actor, words. Opus at " << (kPhraseBitrate / 1000)
        << " kbit/s beside this file, one line each.\n";
    for (const std::string& line : lines) out << line << "\n";
    return {path, bytes.size()};
}

}  // namespace moonshine
