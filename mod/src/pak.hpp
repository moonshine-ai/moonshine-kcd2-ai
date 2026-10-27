#pragma once

#include <string>
#include <vector>

namespace kcd2 {

// Builds a .pak, which is a zip file under another name. CryPak rejects
// archives carrying ZIP extra fields, and the modding docs ask for no
// compression, so entries are stored verbatim with a bare local header.
class Pak {
public:
    // path_in_pak mirrors the layout of the game's own paks and uses forward
    // slashes regardless of host OS.
    void add(const std::string& path_in_pak, const std::string& contents);

    // Throws std::runtime_error if the file cannot be written.
    void write(const std::string& out_path) const;

    std::size_t entry_count() const { return entries_.size(); }

private:
    struct Entry {
        std::string path;
        std::string data;
    };
    std::vector<Entry> entries_;
};

// Writes a loose file, creating parent directories as needed.
void write_file(const std::string& out_path, const std::string& contents);

}  // namespace kcd2
