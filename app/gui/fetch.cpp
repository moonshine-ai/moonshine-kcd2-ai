#include "fetch.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#include <mbedtls/sha256.h>

#include "httplib.h"
#include "json.hpp"
#include "child.h"

namespace moonshine::gui {

namespace fs = std::filesystem;

namespace {

constexpr const char* kAgent = "Moonshine-Fetch/1.0";

struct Address {
    // "https://host:port", which is what the HTTP client is made from.
    std::string origin;
    // The rest, from the first slash.
    std::string path;
};

std::optional<Address> address_of(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::nullopt;
    const std::string name = url.substr(0, scheme);
    if (name != "http" && name != "https") return std::nullopt;
    const std::size_t slash = url.find('/', scheme + 3);
    Address address;
    address.origin = url.substr(0, slash);
    address.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (address.origin.size() <= scheme + 3) return std::nullopt;
    return address;
}

std::unique_ptr<httplib::Client> client_for(const Address& address, int stall_seconds) {
    auto client = std::make_unique<httplib::Client>(address.origin);
    // The files are on a CDN the published address redirects to.
    client->set_follow_location(true);
    client->set_connection_timeout(20, 0);
    client->set_write_timeout(30, 0);
    // No limit on the whole: a 2.5 GB file on a slow line takes what it takes. A transfer
    // that stops moving is what is caught.
    client->set_read_timeout(stall_seconds, 0);
    client->set_default_headers({{"User-Agent", kAgent}});
    return client;
}

std::string megabytes(std::uint64_t bytes) { return std::to_string((bytes + 512 * 1024) / (1024 * 1024)); }

// A path from a manifest is relative and stays inside the installation, whatever the
// manifest says: it is fetched from the network.
bool safe_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\') return false;
    if (path.find(':') != std::string::npos) return false;
    std::string piece;
    for (const char c : path) {
        if (c == '/' || c == '\\') {
            if (piece == ".." || piece.empty()) return false;
            piece.clear();
        } else {
            piece += c;
        }
    }
    return piece != ".." && !piece.empty();
}

std::uint64_t size_of(const fs::path& file) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

// One attempt at a download, carrying on from <part> if it is there. Empty when the
// server sent everything it had (the caller checks the size and the hash), otherwise why
// not. `moved` is told how much of the file is now on disk, and stops the transfer by
// saying false.
std::string download_once(const std::string& url, const fs::path& part, std::uint64_t expected, int stall_seconds,
                          const std::function<bool(std::uint64_t)>& moved) {
    std::uint64_t have = size_of(part);
    if (expected > 0 && have >= expected) return {};
    const std::optional<Address> address = address_of(url);
    if (!address) return "not an http(s) address: " + url;
    std::unique_ptr<httplib::Client> client = client_for(*address, stall_seconds);

    httplib::Headers headers;
    if (have > 0) headers.emplace("Range", "bytes=" + std::to_string(have) + "-");

    std::ofstream out;
    std::string failure;
    bool nothing_more = false;
    const httplib::Result result = client->Get(
        address->path, headers,
        [&](const httplib::Response& response) {
            if (response.status == 206) {
                out.open(part, std::ios::binary | std::ios::app);
            } else if (response.status == 200) {
                // The server ignored the range: start over.
                have = 0;
                out.open(part, std::ios::binary | std::ios::trunc);
            } else if (response.status == 416 && have > 0) {
                // Nothing past what is here; the size check decides.
                nothing_more = true;
                return false;
            } else {
                failure = "HTTP " + std::to_string(response.status);
                return false;
            }
            if (!out) {
                failure = "could not write " + utf8_of(part);
                return false;
            }
            return true;
        },
        [&](const char* data, std::size_t count) {
            out.write(data, static_cast<std::streamsize>(count));
            if (!out) {
                failure = "could not write " + utf8_of(part) + " (is the disk full?)";
                return false;
            }
            have += count;
            return moved(have);
        });
    if (out.is_open()) {
        out.flush();
        if (!out && failure.empty()) failure = "could not write " + utf8_of(part) + " (is the disk full?)";
        out.close();
    }
    if (!failure.empty()) return failure;
    if (nothing_more) return {};
    if (!result) {
        switch (result.error()) {
            case httplib::Error::Canceled: return "stopped";
            case httplib::Error::Read:
            case httplib::Error::Timeout: return "the transfer stalled";
            case httplib::Error::Connection:
            case httplib::Error::ConnectionTimeout: return "could not reach the server";
            default: return "the transfer failed (" + httplib::to_string(result.error()) + ")";
        }
    }
    return {};
}

}  // namespace

std::optional<Manifest> parse_manifest(std::string_view text, std::string* trouble) {
    const auto fail = [&](const char* why) {
        if (trouble) *trouble = why;
        return std::nullopt;
    };
    // A file PowerShell wrote may begin with a byte-order mark.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    const nlohmann::json found = nlohmann::json::parse(text, nullptr, false);
    if (!found.is_object()) return fail("the manifest is not JSON");
    const auto word = [](const nlohmann::json& in, const char* key) {
        const auto it = in.find(key);
        return it != in.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const auto file_of = [&](const nlohmann::json& in) {
        ManifestFile file;
        file.path = word(in, "path");
        file.sha256 = word(in, "sha256");
        std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        file.url = word(in, "url");
        const auto bytes = in.find("bytes");
        if (bytes != in.end() && bytes->is_number_integer() && bytes->get<std::int64_t>() >= 0) {
            file.bytes = static_cast<std::uint64_t>(bytes->get<std::int64_t>());
        }
        return file;
    };
    Manifest manifest;
    manifest.version = word(found, "version");
    manifest.latest = word(found, "latest");
    if (const auto setup = found.find("setup"); setup != found.end() && setup->is_object()) {
        manifest.setup = file_of(*setup);
    }
    if (const auto files = found.find("files"); files != found.end() && files->is_array()) {
        for (const nlohmann::json& entry : *files) {
            if (!entry.is_object()) continue;
            ManifestFile file = file_of(entry);
            if (!safe_path(file.path)) return fail("the manifest names a file outside the installation");
            if (file.url.empty() || file.sha256.size() != 64) return fail("the manifest has a file without an address or a hash");
            manifest.files.push_back(std::move(file));
        }
    }
    if (manifest.version.empty()) return fail("the manifest does not say which version it is");
    return manifest;
}

std::optional<Manifest> read_manifest(const fs::path& file, std::string* trouble) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (trouble) *trouble = "no manifest at " + utf8_of(file);
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_manifest(text.str(), trouble);
}

int compare_versions(std::string_view a, std::string_view b) {
    const auto pieces = [](std::string_view text) {
        std::vector<long long> out;
        std::string piece;
        const auto close = [&] {
            // A piece that is not wholly a number is 0, as [int]::TryParse leaves it.
            const bool number = !piece.empty() && piece.size() <= 9 &&
                                std::all_of(piece.begin(), piece.end(), [](unsigned char c) { return std::isdigit(c); });
            out.push_back(number ? std::stoll(piece) : 0);
            piece.clear();
        };
        for (const char c : text) {
            if (c == '.' || c == '-') {
                close();
            } else if (!std::isspace(static_cast<unsigned char>(c))) {
                piece += c;
            }
        }
        close();
        return out;
    };
    const std::vector<long long> left = pieces(a);
    const std::vector<long long> right = pieces(b);
    for (std::size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        const long long x = i < left.size() ? left[i] : 0;
        const long long y = i < right.size() ? right[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::string sha256_of(const fs::path& file, const std::function<bool()>& cancelled,
                      const std::function<void(std::uint64_t)>& read) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    std::vector<char> buffer(1024 * 1024);
    std::uint64_t done = 0;
    bool stopped = false;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>(buffer.data()),
                              static_cast<std::size_t>(got));
        done += static_cast<std::uint64_t>(got);
        if (read) read(done);
        if (cancelled && cancelled()) {
            stopped = true;
            break;
        }
    }
    unsigned char digest[32];
    mbedtls_sha256_finish(&context, digest);
    mbedtls_sha256_free(&context);
    if (stopped || in.bad()) return {};
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (const unsigned char byte : digest) {
        out += kHex[byte >> 4];
        out += kHex[byte & 15];
    }
    return out;
}

FetchResult fetch_files(const Manifest& manifest, const FetchOptions& options) {
    FetchResult result;
    const auto log = [&](const std::string& line) {
        if (options.log) options.log(line);
    };
    const auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    const auto pause = [&](int seconds) {
        if (options.pause) {
            options.pause(seconds);
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < until && !cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    Progress progress;
    progress.count = static_cast<int>(manifest.files.size());
    const auto show = [&] {
        if (options.progress) options.progress(progress);
    };

    std::uint64_t all = 0;
    for (const ManifestFile& file : manifest.files) all += file.bytes;
    log("Moonshine " + manifest.version + ": " + std::to_string(manifest.files.size()) + " files, " + megabytes(all) +
        " MB, into " + utf8_of(options.dest));

    // First what is already right, which is most of it and decides how much there is to
    // fetch; the size is looked at before the hash, since a 2.5 GB hash is seconds and a
    // wrong size settles it without one.
    progress.phase = "checking";
    progress.total = all;
    std::vector<const ManifestFile*> wanted;
    std::uint64_t checked = 0;
    for (std::size_t i = 0; i < manifest.files.size(); ++i) {
        const ManifestFile& file = manifest.files[i];
        const fs::path target = options.dest / path_from_utf8(file.path);
        progress.file = file.path;
        progress.index = static_cast<int>(i) + 1;
        progress.file_total = file.bytes;
        progress.file_done = 0;
        progress.done = checked;
        show();
        std::error_code ec;
        std::string state = "missing";
        if (fs::is_regular_file(target, ec)) {
            state = "wrong";
            if (size_of(target) == file.bytes) {
                const std::string got = sha256_of(target, cancelled, [&](std::uint64_t done) {
                    progress.file_done = done;
                    progress.done = checked + done;
                    show();
                });
                if (cancelled()) {
                    result.cancelled = true;
                    log("stopped");
                    return result;
                }
                if (got == file.sha256) state = "ok";
            }
        }
        checked += file.bytes;
        if (state == "ok") {
            result.right += 1;
            log("  ok        " + file.path);
            continue;
        }
        if (options.check_only) {
            result.bad.push_back(file.path);
            log("  " + state + std::string(10 - state.size(), ' ') + file.path);
            continue;
        }
        wanted.push_back(&file);
    }
    if (options.check_only) {
        log(result.bad.empty() ? "all files present and correct"
                               : std::to_string(result.bad.size()) + " file(s) missing or wrong");
        return result;
    }

    std::uint64_t to_fetch = 0;
    for (const ManifestFile* file : wanted) to_fetch += file->bytes;
    progress.phase = "downloading";
    progress.total = to_fetch;
    progress.count = static_cast<int>(wanted.size());
    std::uint64_t before = 0;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const ManifestFile& file = *wanted[i];
        const fs::path target = options.dest / path_from_utf8(file.path);
        fs::path part = target;
        part += ".part";
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        log("  fetching  " + file.path + "  (" + megabytes(file.bytes) + " MB)");

        progress.file = file.path;
        progress.index = static_cast<int>(i) + 1;
        progress.file_total = file.bytes;
        std::string why;
        bool fetched = false;
        for (int attempt = 1; !fetched; ++attempt) {
            progress.phase = "downloading";
            progress.note.clear();
            // The speed over the last couple of seconds, which is what a person watching
            // wants; an average over a resumed gigabyte says little.
            auto window_start = std::chrono::steady_clock::now();
            std::uint64_t window_from = size_of(part);
            auto last_shown = window_start - std::chrono::seconds(1);
            double window_length = 0.5;  // the first figure soon, the ones after it steadier
            why = download_once(file.url, part, file.bytes, options.stall_seconds, [&](std::uint64_t have) {
                const auto now = std::chrono::steady_clock::now();
                const double window = std::chrono::duration<double>(now - window_start).count();
                if (window >= window_length) {
                    progress.bytes_per_second = have >= window_from ? (have - window_from) / window : 0;
                    window_start = now;
                    window_from = have;
                    window_length = 2.0;
                }
                if (now - last_shown >= std::chrono::milliseconds(200)) {
                    progress.file_done = have;
                    progress.done = before + have;
                    show();
                    last_shown = now;
                }
                return !cancelled();
            });
            if (cancelled()) {
                result.cancelled = true;
                log("stopped; what was downloaded is kept and carried on from next time");
                return result;
            }
            bool bad_bytes = false;
            if (why.empty()) {
                const std::uint64_t size = size_of(part);
                if (size != file.bytes) {
                    why = "got " + std::to_string(size) + " bytes, the manifest says " + std::to_string(file.bytes);
                    bad_bytes = true;
                } else {
                    progress.phase = "checking";
                    progress.file_done = file.bytes;
                    progress.done = before + file.bytes;
                    show();
                    const std::string got = sha256_of(part, cancelled);
                    if (cancelled()) {
                        result.cancelled = true;
                        return result;
                    }
                    if (got != file.sha256) {
                        why = "sha256 " + got + ", the manifest says " + file.sha256;
                        bad_bytes = true;
                    }
                }
            }
            if (why.empty()) {
                fs::rename(part, target, ec);
                if (ec) {
                    // In use, most likely: the engine has it open.
                    why = "could not put " + file.path + " in place: " + ec.message();
                } else {
                    fetched = true;
                    break;
                }
            }
            // A wrong size or hash means the bytes there are bad, not short; start again.
            if (bad_bytes) fs::remove(part, ec);
            if (attempt >= options.attempts) {
                why = file.path + ": " + why + ", after " + std::to_string(attempt) + " attempts";
                break;
            }
            const int wait = std::min(60, 5 * attempt);
            log("  retry " + std::to_string(attempt) + "/" + std::to_string(options.attempts) + " for " + file.path +
                " in " + std::to_string(wait) + "s: " + why);
            progress.phase = "waiting";
            progress.note = why;
            progress.bytes_per_second = 0;
            show();
            pause(wait);
            if (cancelled()) {
                result.cancelled = true;
                return result;
            }
        }
        if (fetched) {
            result.fetched += 1;
            before += file.bytes;
            progress.done = before;
            show();
        } else {
            result.bad.push_back(file.path);
            result.trouble = why;
            log("  FAILED    " + why);
        }
    }
    if (!result.bad.empty()) {
        log(std::to_string(result.bad.size()) + " of " + std::to_string(manifest.files.size()) +
            " files could not be fetched. Partial downloads are kept.");
    } else {
        log("done: " + std::to_string(result.right) + " already right, " + std::to_string(result.fetched) +
            " downloaded");
    }
    return result;
}

std::optional<std::string> fetch_text(const std::string& url, std::string* trouble,
                                      const std::function<bool()>& cancelled) {
    const auto fail = [&](const std::string& why) {
        if (trouble) *trouble = why;
        return std::nullopt;
    };
    const std::optional<Address> address = address_of(url);
    if (!address) return fail("not an http(s) address: " + url);
    std::unique_ptr<httplib::Client> client = client_for(*address, 60);
    std::string body;
    int status = 0;
    const httplib::Result result = client->Get(
        address->path, httplib::Headers{},
        [&](const httplib::Response& response) {
            status = response.status;
            return true;
        },
        [&](const char* data, std::size_t count) {
            body.append(data, count);
            // A manifest is a few kilobytes; something that goes on for megabytes is not one.
            return body.size() < 8 * 1024 * 1024 && !(cancelled && cancelled());
        });
    if (!result) {
        if (result.error() == httplib::Error::Connection || result.error() == httplib::Error::ConnectionTimeout) {
            return fail("could not reach the server. Check that this PC is online.");
        }
        return fail("could not read it (" + httplib::to_string(result.error()) + ")");
    }
    if (status != 200) return fail("HTTP " + std::to_string(status));
    return body;
}

bool mirror_folder(const fs::path& from, const fs::path& to, std::string* trouble) {
    const auto fail = [&](const std::string& why) {
        if (trouble) *trouble = why;
        return false;
    };
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return fail("nothing at " + utf8_of(from));
    fs::create_directories(to, ec);
    if (ec) return fail("could not make " + utf8_of(to) + ": " + ec.message());

    const auto same = [](const fs::path& a, const fs::path& b) {
        std::error_code err;
        if (!fs::is_regular_file(b, err) || fs::file_size(a, err) != fs::file_size(b, err)) return false;
        std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
        std::vector<char> x(64 * 1024), y(64 * 1024);
        while (left && right) {
            left.read(x.data(), static_cast<std::streamsize>(x.size()));
            right.read(y.data(), static_cast<std::streamsize>(y.size()));
            if (left.gcount() != right.gcount()) return false;
            if (!std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
        }
        return true;
    };
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path target = to / fs::relative(it->path(), from, ec);
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
        } else if (it->is_regular_file(ec) && !same(it->path(), target)) {
            fs::create_directories(target.parent_path(), ec);
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("could not copy " + utf8_of(target) + ": " + ec.message());
        }
    }
    // And what is there that should not be, deepest first so a folder is empty by the time
    // it is its turn.
    std::vector<fs::path> extra;
    for (fs::recursive_directory_iterator it(to, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code err;
        if (!fs::exists(from / fs::relative(it->path(), to, err), err)) extra.push_back(it->path());
    }
    std::sort(extra.begin(), extra.end(), [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();
    });
    for (const fs::path& path : extra) fs::remove(path, ec);
    return true;
}

}  // namespace moonshine::gui
