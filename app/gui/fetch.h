// Bringing the big files of an installation into place from the manifest that lists them:
// installer/Fetch-Package.ps1, in the app. Repair and Update are both this.
//
// The manifest is the one the installer is built from and installs beside the app:
//
//   { "version": "0.9.123",
//     "latest": "https://.../latest/manifest.json",        where an update is looked for
//     "setup": { "url": "https://.../Moonshine-0.9.123-setup.exe", "bytes": ..., "sha256": "..." },
//     "files": [ { "path": "models/llm/Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
//                  "bytes": 2497281120, "sha256": "3605...597", "url": "https://..." }, ... ] }
//
// The rules are the script's, because they are what has been tested on players' lines:
// a file already there with the right size and sha256 is left alone, so a repair moves
// only what is wrong; a download goes to <path>.part and carries on from where it
// stopped, with a range request; a transfer that stalls for a minute is retried, five
// times with a pause that grows; and the bytes are checked against the manifest before
// the file is put in place, so what is in place is always whole.

#ifndef MOONSHINE_GUI_FETCH_H
#define MOONSHINE_GUI_FETCH_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moonshine::gui {

struct ManifestFile {
    // Relative to the installation, with forward slashes.
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::string url;
};

struct Manifest {
    std::string version;
    // Where the newest manifest is published; empty in one that does not say.
    std::string latest;
    // The installer for this version; its url is empty in a manifest without one.
    ManifestFile setup;
    std::vector<ManifestFile> files;
};

// Nothing, with the reason, for text that is not a manifest.
std::optional<Manifest> parse_manifest(std::string_view json, std::string* trouble = nullptr);
std::optional<Manifest> read_manifest(const std::filesystem::path& file, std::string* trouble = nullptr);

// Dotted numbers compared piece by piece, as Moonshine.ps1's Compare-Version does:
// "0.9.102.28.1" is newer than "0.9.102.28", and a piece that is not a number counts as 0.
// Negative, zero or positive as `a` is older than, the same as or newer than `b`.
int compare_versions(std::string_view a, std::string_view b);

// The sha256 of a file, in lower-case hex; empty when it cannot be read or `cancelled`
// said to stop. `read` is told the bytes gone through so far.
std::string sha256_of(const std::filesystem::path& file, const std::function<bool()>& cancelled = {},
                      const std::function<void(std::uint64_t)>& read = {});

// Where a fetch has got to, for the window.
struct Progress {
    // "checking" what is already there, "downloading", or "waiting" to try again.
    std::string phase;
    // The file in hand, and which of how many.
    std::string file;
    int index = 0;
    int count = 0;
    std::uint64_t file_done = 0;
    std::uint64_t file_total = 0;
    // Over everything that has to be downloaded.
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    double bytes_per_second = 0;
    // Why the last attempt failed, while waiting.
    std::string note;
};

struct FetchOptions {
    // The installation the paths are relative to.
    std::filesystem::path dest;
    // Say what is missing or wrong, and change nothing.
    bool check_only = false;
    std::function<void(const Progress&)> progress;
    std::function<bool()> cancelled;
    // A line for logs/fetch.log, which Collect-Logs.ps1 picks up.
    std::function<void(const std::string&)> log;
    int attempts = 5;
    // How long a transfer may deliver nothing before it is given up as stalled.
    int stall_seconds = 60;
    // The pause before another attempt. A test passes one that does not wait.
    std::function<void(int seconds)> pause;
};

struct FetchResult {
    int right = 0;
    int fetched = 0;
    // The paths that are missing or wrong and could not be (or, checking, were not) put right.
    std::vector<std::string> bad;
    bool cancelled = false;
    // The last failure, in words.
    std::string trouble;
    bool ok() const { return bad.empty() && !cancelled; }
};

FetchResult fetch_files(const Manifest& manifest, const FetchOptions& options);

// A small file over HTTP(S), whole: the published manifest. Nothing, with the reason,
// when it could not be had.
std::optional<std::string> fetch_text(const std::string& url, std::string* trouble = nullptr,
                                      const std::function<bool()>& cancelled = {});

// `to` made the same as `from`: every file copied that differs, anything else there
// removed. How the mod in the installation is put into the game's mods folder.
bool mirror_folder(const std::filesystem::path& from, const std::filesystem::path& to,
                   std::string* trouble = nullptr);

}  // namespace moonshine::gui

#endif  // MOONSHINE_GUI_FETCH_H
