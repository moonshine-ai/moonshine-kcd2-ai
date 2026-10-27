#include "pak.hpp"

#include <miniz.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace kcd2 {
namespace {

// A .pak is a zip under another name, but CryPak is an old reader and the
// shipped paks are conservative: stored or deflated, general-purpose flag 0,
// sizes and CRC in the local header, no extra fields. miniz's writer always
// emits a trailing data descriptor and sets flag bit 3 instead, which no
// shipped pak does, so the headers are written by hand here to match the game.

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEocdSig = 0x06054b50;
constexpr std::uint16_t kVersion = 20;  // 2.0, what the shipped paks declare
constexpr std::uint16_t kStored = 0;

// Fixed at the DOS epoch so a rebuild from an unchanged database produces a
// byte-identical pak and redeploying copies nothing.
constexpr std::uint16_t kDosTime = 0;
constexpr std::uint16_t kDosDate = (1 << 5) | 1;  // 1980-01-01

void put16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}

void put32(std::string& out, std::uint32_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
    out += static_cast<char>((v >> 16) & 0xff);
    out += static_cast<char>((v >> 24) & 0xff);
}

std::uint32_t crc32_of(const std::string& data) {
    return static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const mz_uint8*>(data.data()), data.size()));
}

void ensure_parent_dir(const std::string& out_path) {
    std::filesystem::path p(out_path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }
}

}  // namespace

void Pak::add(const std::string& path_in_pak, const std::string& contents) {
    entries_.push_back({path_in_pak, contents});
}

void Pak::write(const std::string& out_path) const {
    std::string archive;
    struct Placed {
        std::uint32_t offset;
        std::uint32_t crc;
    };
    std::vector<Placed> placed;
    placed.reserve(entries_.size());

    for (const Entry& e : entries_) {
        if (e.data.size() > 0xffffffffu) {
            throw std::runtime_error("entry too large for a 32-bit zip: " + e.path);
        }
        Placed at{static_cast<std::uint32_t>(archive.size()), crc32_of(e.data)};
        placed.push_back(at);

        const auto size = static_cast<std::uint32_t>(e.data.size());
        put32(archive, kLocalSig);
        put16(archive, kVersion);
        put16(archive, 0);  // general purpose flag: no data descriptor, no UTF-8 bit
        put16(archive, kStored);
        put16(archive, kDosTime);
        put16(archive, kDosDate);
        put32(archive, at.crc);
        put32(archive, size);  // compressed size, same as stored size
        put32(archive, size);
        put16(archive, static_cast<std::uint16_t>(e.path.size()));
        put16(archive, 0);  // extra field length
        archive += e.path;
        archive += e.data;
    }

    const auto central_offset = static_cast<std::uint32_t>(archive.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        const auto size = static_cast<std::uint32_t>(e.data.size());
        put32(archive, kCentralSig);
        put16(archive, kVersion);  // version made by
        put16(archive, kVersion);  // version needed to extract
        put16(archive, 0);
        put16(archive, kStored);
        put16(archive, kDosTime);
        put16(archive, kDosDate);
        put32(archive, placed[i].crc);
        put32(archive, size);
        put32(archive, size);
        put16(archive, static_cast<std::uint16_t>(e.path.size()));
        put16(archive, 0);  // extra field length
        put16(archive, 0);  // file comment length
        put16(archive, 0);  // disk number start
        put16(archive, 0);  // internal file attributes
        put32(archive, 0);  // external file attributes
        put32(archive, placed[i].offset);
        archive += e.path;
    }
    const auto central_size = static_cast<std::uint32_t>(archive.size()) - central_offset;

    const auto count = static_cast<std::uint16_t>(entries_.size());
    put32(archive, kEocdSig);
    put16(archive, 0);  // this disk
    put16(archive, 0);  // disk with the central directory
    put16(archive, count);
    put16(archive, count);
    put32(archive, central_size);
    put32(archive, central_offset);
    put16(archive, 0);  // archive comment length

    write_file(out_path, archive);
}

void write_file(const std::string& out_path, const std::string& contents) {
    ensure_parent_dir(out_path);
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw std::runtime_error("cannot write " + out_path);
    }
    f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!f) {
        throw std::runtime_error("write failed for " + out_path);
    }
}

}  // namespace kcd2
