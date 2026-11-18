// Fetching an installation's files from its manifest (gui/fetch.h), against a server in
// this process.
//
// The cases that matter are the ones a player's line produces and a developer's does not:
// a download cut off half way and carried on, a server that ignores the request to carry
// on and sends the whole file again, and bytes that arrive wrong. Each is a server here
// that does exactly that.

#include "doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fetch.h"
#include "httplib.h"
#include "json.hpp"

using namespace moonshine::gui;
namespace fs = std::filesystem;

namespace {

fs::path fresh_folder(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "moonshine-gui-fetch-tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << bytes;
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Bytes that are not the same anywhere along their length, so that a piece put in the
// wrong place is a different file.
std::string content(std::size_t size, unsigned seed) {
    std::string out(size, '\0');
    unsigned state = seed * 2654435761u + 1;
    for (char& c : out) {
        state = state * 1664525u + 1013904223u;
        c = static_cast<char>(state >> 24);
    }
    return out;
}

std::string sha_of(const std::string& bytes, const char* name = "hash-me") {
    const fs::path file = fs::temp_directory_path() / "moonshine-gui-fetch-tests" / name;
    write_file(file, bytes);
    return sha256_of(file);
}

// A file server on a port of its own. What it serves and how it misbehaves is the test's.
class FileServer {
public:
    struct Asked {
        std::string path;
        std::string range;
        std::string agent;
    };

    FileServer() {
        server_.Get(R"(/files/(.+))", [this](const httplib::Request& request, httplib::Response& response) {
            const std::string name = request.matches[1];
            std::string bytes;
            int attempt = 0;
            {
                std::lock_guard<std::mutex> held(lock_);
                asked_.push_back({request.path, request.get_header_value("Range"), request.get_header_value("User-Agent")});
                attempt = ++attempts_[name];
                const auto found = files_.find(name);
                if (found == files_.end()) {
                    response.status = 404;
                    return;
                }
                bytes = found->second;
                // The first so-many answers for a file are damaged, when the test says so.
                if (attempt <= corrupt_first_[name]) bytes[bytes.size() / 2] ^= 0x55;
            }
            if (slow_) {
                // A piece at a time, so that a test can stop the transfer part of the way.
                response.set_content_provider(bytes.size(), "application/octet-stream",
                                              [bytes](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                                                  const std::size_t piece = std::min<std::size_t>(length, 4096);
                                                  sink.write(bytes.data() + offset, piece);
                                                  std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                                  return true;
                                              });
            } else {
                response.set_content(bytes, "application/octet-stream");
            }
        });
        server_.Get("/redirect/(.+)", [this](const httplib::Request& request, httplib::Response& response) {
            response.set_redirect("/files/" + std::string(request.matches[1]));
        });
        server_.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response&) {
            // A server that does not do ranges answers a range request with the whole file.
            if (ignore_ranges_) const_cast<httplib::Request&>(request).ranges.clear();
            return httplib::Server::HandlerResponse::Unhandled;
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~FileServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string url(const std::string& name) const { return "http://127.0.0.1:" + std::to_string(port_) + "/files/" + name; }
    std::string redirect(const std::string& name) const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/redirect/" + name;
    }

    void serve(const std::string& name, const std::string& bytes) {
        std::lock_guard<std::mutex> held(lock_);
        files_[name] = bytes;
    }
    void corrupt_first(const std::string& name, int times) {
        std::lock_guard<std::mutex> held(lock_);
        corrupt_first_[name] = times;
    }
    void ignore_ranges(bool on) { ignore_ranges_ = on; }
    void slow(bool on) { slow_ = on; }
    std::vector<Asked> asked() {
        std::lock_guard<std::mutex> held(lock_);
        return asked_;
    }

    // A manifest entry for a file this server has.
    ManifestFile entry(const std::string& path, const std::string& name, const std::string& bytes) {
        serve(name, bytes);
        return {path, bytes.size(), sha_of(bytes), url(name)};
    }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    std::mutex lock_;
    std::map<std::string, std::string> files_;
    std::map<std::string, int> attempts_;
    std::map<std::string, int> corrupt_first_;
    std::vector<Asked> asked_;
    std::atomic<bool> ignore_ranges_{false};
    std::atomic<bool> slow_{false};
};

FetchOptions options_for(const fs::path& dest, std::vector<int>* pauses = nullptr) {
    FetchOptions options;
    options.dest = dest;
    options.stall_seconds = 5;
    options.pause = [pauses](int seconds) {
        if (pauses) pauses->push_back(seconds);
    };
    return options;
}

}  // namespace

// --- the manifest ------------------------------------------------------------------------------

TEST_CASE("manifest: the one the installer ships") {
    std::string trouble;
    // As PowerShell writes it: a byte-order mark first, and the hashes in capitals.
    const std::string text = "\xEF\xBB\xBF" R"({
          "version": "0.9.123",
          "latest": "https://download.example/latest/manifest.json",
          "setup": { "url": "https://download.example/Moonshine-0.9.123-setup.exe", "bytes": 101, "sha256": "AB12" },
          "files": [
            { "path": "models/llm/model.gguf", "bytes": 2497281120,
              "sha256": "3605)" + std::string(57, 'A') + R"(597",
              "url": "https://huggingface.example/model.gguf" }
          ] })";
    const std::optional<Manifest> manifest = parse_manifest(text, &trouble);
    REQUIRE_MESSAGE(manifest, trouble);
    CHECK(manifest->version == "0.9.123");
    CHECK(manifest->latest == "https://download.example/latest/manifest.json");
    CHECK(manifest->setup.url == "https://download.example/Moonshine-0.9.123-setup.exe");
    CHECK(manifest->setup.bytes == 101);
    REQUIRE(manifest->files.size() == 1);
    // More than fits in 32 bits, which the model is.
    CHECK(manifest->files[0].bytes == 2497281120ull);
    // Compared as text against what this program computes, which is lower case.
    CHECK(manifest->files[0].sha256 == "3605" + std::string(57, 'a') + "597");
}

TEST_CASE("manifest: one that would write outside the installation is refused whole") {
    const std::string hash(64, 'a');
    for (const char* path : {"../elsewhere.dll", "models/../../x", "C:/Windows/x.dll", "/etc/passwd", "\\\\server\\share\\x",
                             "models//x", ""}) {
        const nlohmann::json file = {{"path", path}, {"bytes", 1}, {"sha256", hash}, {"url", "http://x/y"}};
        const std::string text = nlohmann::json{{"version", "1"}, {"files", {file}}}.dump();
        std::string trouble;
        CHECK_MESSAGE(!parse_manifest(text, &trouble), path);
    }
    CHECK_FALSE(parse_manifest("not json"));
    CHECK_FALSE(parse_manifest(R"({"files":[]})"));
    CHECK_FALSE(parse_manifest(R"({"version":"1","files":[{"path":"a","bytes":1,"sha256":"short","url":"http://x/y"}]})"));
    CHECK_FALSE(parse_manifest(R"({"version":"1","files":[{"path":"a","bytes":1,"sha256":")" + hash + R"("}]})"));
}

TEST_CASE("versions: compared as Moonshine.ps1 compares them") {
    CHECK(compare_versions("0.9.102.28", "0.9.102.28") == 0);
    CHECK(compare_versions("0.9.103", "0.9.102.28") > 0);
    CHECK(compare_versions("0.9.102.28", "0.9.103") < 0);
    // A fifth number is a rebuild of the same release, and newer than it.
    CHECK(compare_versions("0.9.102.28.1", "0.9.102.28") > 0);
    // Numbers, not text: 10 is after 9.
    CHECK(compare_versions("0.10.0", "0.9.99") > 0);
    CHECK(compare_versions("1.0-2", "1.0.1") > 0);
    // A piece that is not a number is 0.
    CHECK(compare_versions("1.0.beta", "1.0") == 0);
    CHECK(compare_versions(" 1.2\n", "1.2") == 0);
    CHECK(compare_versions("dev", "0.0.1") < 0);
}

TEST_CASE("sha256: the standard's own example, and a file that is not there") {
    CHECK(sha_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_of(fs::temp_directory_path() / "moonshine-gui-fetch-tests" / "no-such-file").empty());
}

// --- the files -----------------------------------------------------------------------------------

TEST_CASE("fetch: what is missing is downloaded, what is right is left alone") {
    FileServer server;
    const fs::path dest = fresh_folder("plain");
    const std::string model = content(300000, 1), voices = content(70000, 2);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("models/llm/model.gguf", "model", model),
                      server.entry("data/voices.bin", "voices", voices)};
    write_file(dest / "data" / "voices.bin", voices);

    std::vector<Progress> seen;
    std::vector<std::string> log;
    FetchOptions options = options_for(dest);
    options.progress = [&](const Progress& progress) { seen.push_back(progress); };
    options.log = [&](const std::string& line) { log.push_back(line); };
    const FetchResult result = fetch_files(manifest, options);

    CHECK(result.ok());
    CHECK(result.right == 1);
    CHECK(result.fetched == 1);
    CHECK(read_file(dest / "models" / "llm" / "model.gguf") == model);
    CHECK_FALSE(fs::exists(dest / "models" / "llm" / "model.gguf.part"));
    // Only the one that was missing was asked for.
    const std::vector<FileServer::Asked> asked = server.asked();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].path == "/files/model");
    CHECK(asked[0].range.empty());
    CHECK(asked[0].agent == "Moonshine-Fetch/1.0");
    // The total is of what has to be fetched, not of everything the manifest lists.
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front().phase == "checking");
    CHECK(seen.front().total == model.size() + voices.size());
    const auto downloading = std::find_if(seen.begin(), seen.end(), [](const Progress& p) { return p.phase == "downloading"; });
    REQUIRE(downloading != seen.end());
    CHECK(downloading->file == "models/llm/model.gguf");
    CHECK(downloading->total == model.size());
    CHECK(downloading->count == 1);
    CHECK(seen.back().total == model.size());
    CHECK(seen.back().done == model.size());
    CHECK(log.back() == "done: 1 already right, 1 downloaded");
}

TEST_CASE("fetch: a download that was cut off carries on from where it stopped") {
    FileServer server;
    const fs::path dest = fresh_folder("resume");
    const std::string model = content(200000, 3);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("model.gguf", "model", model)};
    write_file(dest / "model.gguf.part", model.substr(0, 120000));

    const FetchResult result = fetch_files(manifest, options_for(dest));
    CHECK(result.ok());
    CHECK(read_file(dest / "model.gguf") == model);
    const std::vector<FileServer::Asked> asked = server.asked();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].range == "bytes=120000-");
}

TEST_CASE("fetch: a server that ignores the range sends the whole file, which is taken as that") {
    FileServer server;
    server.ignore_ranges(true);
    const fs::path dest = fresh_folder("no-ranges");
    const std::string model = content(200000, 4);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("model.gguf", "model", model)};
    write_file(dest / "model.gguf.part", model.substr(0, 120000));

    std::vector<int> pauses;
    const FetchResult result = fetch_files(manifest, options_for(dest, &pauses));
    CHECK(result.ok());
    // Not the part with the whole file after it.
    CHECK(read_file(dest / "model.gguf") == model);
    CHECK(server.asked().size() == 1);
    CHECK(pauses.empty());
}

TEST_CASE("fetch: a part that is already the whole file is checked and put in place without asking") {
    FileServer server;
    const fs::path dest = fresh_folder("whole-part");
    const std::string model = content(50000, 5);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("model.gguf", "model", model)};
    write_file(dest / "model.gguf.part", model);
    const FetchResult result = fetch_files(manifest, options_for(dest));
    CHECK(result.ok());
    CHECK(read_file(dest / "model.gguf") == model);
    CHECK(server.asked().empty());
}

TEST_CASE("fetch: bytes that arrive wrong are thrown away and fetched again") {
    FileServer server;
    const fs::path dest = fresh_folder("wrong-once");
    const std::string model = content(90000, 6);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("model.gguf", "model", model)};
    server.corrupt_first("model", 1);

    std::vector<int> pauses;
    std::vector<std::string> log;
    FetchOptions options = options_for(dest, &pauses);
    options.log = [&](const std::string& line) { log.push_back(line); };
    const FetchResult result = fetch_files(manifest, options);
    CHECK(result.ok());
    CHECK(read_file(dest / "model.gguf") == model);
    // The second try started from nothing: a wrong hash means the bytes are bad, not short.
    const std::vector<FileServer::Asked> asked = server.asked();
    REQUIRE(asked.size() == 2);
    CHECK(asked[1].range.empty());
    CHECK(pauses == std::vector<int>{5});
    bool said = false;
    for (const std::string& line : log) said = said || line.find("retry 1/5 for model.gguf in 5s: sha256") != std::string::npos;
    CHECK(said);
}

TEST_CASE("fetch: a file that never arrives right is never put in place") {
    FileServer server;
    const fs::path dest = fresh_folder("always-wrong");
    const std::string good = content(40000, 7), model = content(40000, 8);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("model.gguf", "model", model), server.entry("good.bin", "good", good)};
    server.corrupt_first("model", 99);
    // What is installed now stays until something right replaces it.
    write_file(dest / "model.gguf", "the old one");

    std::vector<int> pauses;
    const FetchResult result = fetch_files(manifest, options_for(dest, &pauses));
    CHECK_FALSE(result.ok());
    CHECK(result.bad == std::vector<std::string>{"model.gguf"});
    CHECK(result.trouble.find("after 5 attempts") != std::string::npos);
    CHECK(read_file(dest / "model.gguf") == "the old one");
    CHECK_FALSE(fs::exists(dest / "model.gguf.part"));
    // Five tries, with the pause growing between them, and the rest still fetched.
    CHECK(pauses == std::vector<int>{5, 10, 15, 20});
    CHECK(result.fetched == 1);
    CHECK(read_file(dest / "good.bin") == good);
}

TEST_CASE("fetch: a file the server does not have is said, with what the server said") {
    FileServer server;
    const fs::path dest = fresh_folder("gone");
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {{"model.gguf", 10, std::string(64, 'a'), server.url("nothing-here")}};
    FetchOptions options = options_for(dest);
    options.attempts = 2;
    const FetchResult result = fetch_files(manifest, options);
    CHECK_FALSE(result.ok());
    CHECK(result.trouble.find("HTTP 404") != std::string::npos);
    CHECK_FALSE(fs::exists(dest / "model.gguf"));
}

TEST_CASE("fetch: nothing listening is a failure after the tries, not a hang") {
    int port = 0;
    {
        FileServer gone;
        const std::string url = gone.url("x");
        port = std::stoi(url.substr(url.find(':', 7) + 1));
    }
    const fs::path dest = fresh_folder("unreachable");
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {{"model.gguf", 10, std::string(64, 'a'), "http://127.0.0.1:" + std::to_string(port) + "/files/x"}};
    FetchOptions options = options_for(dest);
    options.attempts = 2;
    const FetchResult result = fetch_files(manifest, options);
    CHECK_FALSE(result.ok());
    CHECK(result.trouble.find("could not reach the server") != std::string::npos);
}

TEST_CASE("fetch: an address that redirects is followed, as the published ones do") {
    FileServer server;
    const fs::path dest = fresh_folder("redirect");
    const std::string model = content(30000, 9);
    server.serve("model", model);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {{"model.gguf", model.size(), sha_of(model), server.redirect("model")}};
    const FetchResult result = fetch_files(manifest, options_for(dest));
    CHECK(result.ok());
    CHECK(read_file(dest / "model.gguf") == model);
}

TEST_CASE("fetch: checking says what is missing and what is wrong, and changes nothing") {
    FileServer server;
    const fs::path dest = fresh_folder("check");
    const std::string a = content(1000, 10), b = content(1000, 11), c = content(1000, 12);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("a.bin", "a", a), server.entry("b.bin", "b", b), server.entry("c.bin", "c", c)};
    write_file(dest / "a.bin", a);
    write_file(dest / "b.bin", "not what it should be");

    FetchOptions options = options_for(dest);
    options.check_only = true;
    std::vector<std::string> log;
    options.log = [&](const std::string& line) { log.push_back(line); };
    const FetchResult result = fetch_files(manifest, options);
    CHECK_FALSE(result.ok());
    CHECK(result.right == 1);
    CHECK(result.bad == std::vector<std::string>{"b.bin", "c.bin"});
    CHECK(server.asked().empty());
    CHECK(read_file(dest / "b.bin") == "not what it should be");
    CHECK_FALSE(fs::exists(dest / "c.bin"));
    CHECK(std::find(log.begin(), log.end(), "  wrong     b.bin") != log.end());
    CHECK(std::find(log.begin(), log.end(), "  missing   c.bin") != log.end());
    CHECK(log.back() == "2 file(s) missing or wrong");
}

TEST_CASE("fetch: stopped part of the way, what has arrived is kept for next time") {
    FileServer server;
    server.slow(true);
    const fs::path dest = fresh_folder("cancel");
    const std::string model = content(400000, 13);
    Manifest manifest;
    manifest.version = "1.0";
    manifest.files = {server.entry("model.gguf", "model", model)};

    // Stopped by the player once some of it is on the disk.
    FetchOptions options = options_for(dest);
    options.cancelled = [&] {
        std::error_code ec;
        const std::uintmax_t have = fs::file_size(dest / "model.gguf.part", ec);
        return !ec && have >= 40000;
    };
    const FetchResult result = fetch_files(manifest, options);
    CHECK(result.cancelled);
    CHECK_FALSE(result.ok());
    CHECK_FALSE(fs::exists(dest / "model.gguf"));
    const std::uintmax_t kept = fs::file_size(dest / "model.gguf.part");
    CHECK(kept >= 40000);
    CHECK(kept < model.size());

    // And next time carries on from there.
    server.slow(false);
    const FetchResult again = fetch_files(manifest, options_for(dest));
    CHECK(again.ok());
    CHECK(read_file(dest / "model.gguf") == model);
    CHECK(server.asked().back().range == "bytes=" + std::to_string(kept) + "-");
}

TEST_CASE("fetch: the published manifest, read as text") {
    FileServer server;
    server.serve("manifest.json", R"({"version":"2.0","files":[]})");
    std::string trouble;
    const std::optional<std::string> text = fetch_text(server.url("manifest.json"), &trouble);
    REQUIRE_MESSAGE(text, trouble);
    CHECK(parse_manifest(*text)->version == "2.0");
    CHECK_FALSE(fetch_text(server.url("no-such"), &trouble));
    CHECK(trouble == "HTTP 404");
    CHECK_FALSE(fetch_text("ftp://example.com/manifest.json", &trouble));
    CHECK(trouble.find("not an http(s) address") != std::string::npos);
}

// --- a bug report, up ----------------------------------------------------------------------------

namespace {

// What the report service is to the app: one address that takes a POST. What it answers
// is the test's.
class ReportServer {
public:
    struct Got {
        std::string body;
        std::string form;
        std::string type;
        std::string length;
        std::string agent;
    };

    ReportServer() {
        server_.Post("/kcd2/report", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::mutex> held(lock_);
            got_.push_back({request.body, request.get_header_value("X-Moonshine-Report"),
                            request.get_header_value("Content-Type"), request.get_header_value("Content-Length"),
                            request.get_header_value("User-Agent")});
            response.status = status_;
            if (status_ >= 300 && status_ < 400) response.set_header("Location", "/elsewhere");
            response.set_content(answer_, "application/json");
        });
        server_.Get("/elsewhere", [this](const httplib::Request&, httplib::Response& response) {
            std::lock_guard<std::mutex> held(lock_);
            followed_ = true;
            response.set_content("{\"ref\":\"WRONG\"}", "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~ReportServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/kcd2/report"; }
    void answer(int status, const std::string& body) {
        std::lock_guard<std::mutex> held(lock_);
        status_ = status;
        answer_ = body;
    }
    std::vector<Got> got() {
        std::lock_guard<std::mutex> held(lock_);
        return got_;
    }
    bool followed() {
        std::lock_guard<std::mutex> held(lock_);
        return followed_;
    }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    std::mutex lock_;
    int status_ = 200;
    std::string answer_ = "{\"ref\":\"AB12CD34\",\"emailed\":true}\n";
    std::vector<Got> got_;
    bool followed_ = false;
};

}  // namespace

TEST_CASE("upload: the file is the body, whole, and the headers go with it") {
    ReportServer server;
    const fs::path dir = fresh_folder("upload");
    // More than one piece of it, and not a whole number of them.
    const std::string zip = content(300001, 21);
    write_file(dir / "logs.zip", zip);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
    const Upload sent = upload_file(server.url(), dir / "logs.zip", {{"X-Moonshine-Report", base64("{\"name\":\"Jind\xC5\x99ich\"}")}},
                                    [&](std::uint64_t done, std::uint64_t total) { seen.emplace_back(done, total); });
    CHECK(sent.ok);
    CHECK(sent.status == 200);
    CHECK(sent.trouble.empty());
    CHECK(sent.body.find("\"ref\":\"AB12CD34\"") != std::string::npos);

    const std::vector<ReportServer::Got> got = server.got();
    REQUIRE(got.size() == 1);
    CHECK(got[0].body == zip);
    CHECK(got[0].length == "300001");
    CHECK(got[0].type == "application/zip");
    CHECK(got[0].form == "eyJuYW1lIjoiSmluZMWZaWNoIn0=");
    CHECK(got[0].agent == "Moonshine-Fetch/1.0");
    // Told as it goes, in order, and at the end all of it.
    REQUIRE(seen.size() >= 2);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK(seen.back() == std::make_pair<std::uint64_t, std::uint64_t>(300001, 300001));
}

TEST_CASE("upload: a refusal is the server's status and what it said") {
    ReportServer server;
    server.answer(429, R"({"error":"too-fast","message":"Too many reports in a minute."})");
    const fs::path dir = fresh_folder("upload-refused");
    write_file(dir / "logs.zip", content(5000, 22));
    const Upload sent = upload_file(server.url(), dir / "logs.zip", {});
    CHECK_FALSE(sent.ok);
    CHECK_FALSE(sent.cancelled);
    CHECK(sent.status == 429);
    CHECK(sent.trouble == "HTTP 429");
    CHECK(sent.body.find("Too many reports in a minute.") != std::string::npos);
}

TEST_CASE("upload: a redirect is not followed, since what follows one is not the report") {
    ReportServer server;
    server.answer(302, "");
    const fs::path dir = fresh_folder("upload-redirect");
    write_file(dir / "logs.zip", content(5000, 23));
    const Upload sent = upload_file(server.url(), dir / "logs.zip", {});
    CHECK_FALSE(sent.ok);
    CHECK(sent.status == 302);
    CHECK_FALSE(server.followed());
}

TEST_CASE("upload: stopped part of the way, the server is never given a report") {
    ReportServer server;
    const fs::path dir = fresh_folder("upload-cancel");
    write_file(dir / "logs.zip", content(2000000, 24));
    std::uint64_t gone = 0;
    const Upload sent = upload_file(server.url(), dir / "logs.zip", {},
                                    [&](std::uint64_t done, std::uint64_t) { gone = done; },
                                    [&] { return gone >= 500000; });
    CHECK(sent.cancelled);
    CHECK_FALSE(sent.ok);
    CHECK(sent.trouble.empty());
    CHECK(gone < 2000000);
    CHECK(server.got().empty());
}

TEST_CASE("upload: nothing to send, nowhere to send it, and nothing listening") {
    ReportServer server;
    const fs::path dir = fresh_folder("upload-nothing");
    Upload sent = upload_file(server.url(), dir / "no-such.zip", {});
    CHECK_FALSE(sent.ok);
    CHECK(sent.trouble.find("could not read") != std::string::npos);
    write_file(dir / "empty.zip", "");
    sent = upload_file(server.url(), dir / "empty.zip", {});
    CHECK(sent.trouble.find("could not read") != std::string::npos);
    CHECK(server.got().empty());

    write_file(dir / "logs.zip", content(1000, 25));
    sent = upload_file("ftp://example.com/report", dir / "logs.zip", {});
    CHECK(sent.trouble.find("not an http(s) address") != std::string::npos);

    std::string gone;
    {
        ReportServer closed;
        gone = closed.url();
    }
    sent = upload_file(gone, dir / "logs.zip", {});
    CHECK_FALSE(sent.ok);
    CHECK(sent.status == 0);
    CHECK(sent.trouble.find("could not reach the server") != std::string::npos);
}

TEST_CASE("base64: the standard's examples") {
    CHECK(base64("") == "");
    CHECK(base64("f") == "Zg==");
    CHECK(base64("fo") == "Zm8=");
    CHECK(base64("foo") == "Zm9v");
    CHECK(base64("foob") == "Zm9vYg==");
    CHECK(base64("fooba") == "Zm9vYmE=");
    CHECK(base64("foobar") == "Zm9vYmFy");
    CHECK(base64(std::string("\xFF\xFE\x00", 3)) == "//4A");
}

// --- the mod, into the game ----------------------------------------------------------------------

TEST_CASE("mirror: the game's copy of the mod is made the installation's") {
    const fs::path home = fresh_folder("mirror");
    const fs::path ours = home / "mod" / "moonshine_names", theirs = home / "game" / "mods" / "moonshine_names";
    write_file(ours / "mod.manifest", "<mod version 2/>");
    write_file(ours / "Data" / "names.pak", "new pak");
    write_file(ours / "Data" / "Scripts" / "bridge.lua", "-- new");
    write_file(theirs / "mod.manifest", "<mod version 1/>");
    write_file(theirs / "Data" / "names.pak", "old pak");
    write_file(theirs / "Data" / "old-only.pak", "left over");
    write_file(theirs / "Gone" / "deep" / "file.txt", "left over");

    std::string trouble;
    REQUIRE_MESSAGE(mirror_folder(ours, theirs, &trouble), trouble);
    CHECK(read_file(theirs / "mod.manifest") == "<mod version 2/>");
    CHECK(read_file(theirs / "Data" / "names.pak") == "new pak");
    CHECK(read_file(theirs / "Data" / "Scripts" / "bridge.lua") == "-- new");
    CHECK_FALSE(fs::exists(theirs / "Data" / "old-only.pak"));
    CHECK_FALSE(fs::exists(theirs / "Gone"));
    // The source is not touched.
    CHECK(read_file(ours / "Data" / "names.pak") == "new pak");

    CHECK_FALSE(mirror_folder(home / "no-such-folder", theirs, &trouble));
}
