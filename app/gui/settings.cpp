#include "settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace moonshine::gui {

namespace {

constexpr std::string_view kMark = "\xEF\xBB\xBF";

bool ascii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80; });
}

[[maybe_unused]] bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const std::size_t more = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : 9;
        if (more == 9 || i + more + 1 > text.size()) return false;
        for (std::size_t k = 1; k <= more; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += more + 1;
    }
    return true;
}

// The file as UTF-8, whoever wrote it. The installer writes the game's folder in the
// system's own encoding (it is an Inno Setup script), which is not UTF-8 for a folder
// with an accent in its name.
std::string decoded(std::string text) {
#ifdef _WIN32
    if (!valid_utf8(text)) {
        const int wide = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring between(static_cast<std::size_t>(wide), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), between.data(), wide);
        const int narrow = WideCharToMultiByte(CP_UTF8, 0, between.data(), wide, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(narrow), '\0');
        WideCharToMultiByte(CP_UTF8, 0, between.data(), wide, out.data(), narrow, nullptr, nullptr);
        return out;
    }
#endif
    return text;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return decoded(text.str());
}

std::string trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// One line, whatever was typed or pasted: a value cannot span lines in this file.
std::string one_line(std::string_view text) {
    std::string out = trimmed(text);
    for (char& c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

bool truthy(const std::string& value, bool otherwise) {
    const std::string v = lowered(value);
    if (v == "on" || v == "yes" || v == "true" || v == "1") return true;
    if (v == "off" || v == "no" || v == "false" || v == "0") return false;
    return otherwise;
}

// The key of a `key = value` line, lowered; empty for a comment, a blank or anything else.
std::string key_of(std::string_view raw, std::string* value = nullptr) {
    const std::string line = trimmed(raw);
    if (line.empty() || line.front() == '#') return {};
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0) return {};
    if (value) *value = trimmed(std::string_view(line).substr(equals + 1));
    return lowered(trimmed(std::string_view(line).substr(0, equals)));
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t from = 0;
    while (from <= text.size()) {
        std::size_t end = text.find('\n', from);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(from, end - from);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.emplace_back(line);
        from = end + 1;
    }
    // A file that ends in a newline does not have an empty last line.
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

// Every setting as the file spells it, with whether it is at its default (and so is not
// written). In the order new keys are added.
struct Entry {
    const char* key;
    std::string value;
    bool is_default;
};

std::vector<Entry> entries_of(const Settings& s) {
    const Settings d;
    const auto word = [](bool on) { return std::string(on ? "on" : "off"); };
    return {
        {"root", one_line(s.root), one_line(s.root).empty()},
        {"args", one_line(s.args), one_line(s.args).empty()},
        {"mic", word(s.mic), s.mic == d.mic},
        {"launch_game", word(s.launch_game), s.launch_game == d.launch_game},
        {"autostart", word(s.autostart), s.autostart == d.autostart},
        {"model", s.api ? "api" : "local", s.api == d.api},
        // The API's own settings are kept when the local model is chosen again, so that
        // going back to the API is one click and not a form to fill in again.
        {"api_url", one_line(s.api_url), one_line(s.api_url).empty()},
        {"api_model", one_line(s.api_model), one_line(s.api_model).empty()},
        {"api_extra", one_line(s.api_extra), one_line(s.api_extra).empty()},
        {"api_guessing", word(s.api_guessing), s.api_guessing == d.api_guessing},
        {"experimental_quests", word(s.experimental_quests), s.experimental_quests == d.experimental_quests},
        {"report_name", one_line(s.report_name), one_line(s.report_name).empty()},
        {"report_email", one_line(s.report_email), one_line(s.report_email).empty()},
    };
}

}  // namespace

bool Settings::operator==(const Settings& o) const {
    return root == o.root && args == o.args && mic == o.mic && launch_game == o.launch_game &&
           autostart == o.autostart && api == o.api && api_url == o.api_url && api_model == o.api_model &&
           api_extra == o.api_extra && api_guessing == o.api_guessing &&
           experimental_quests == o.experimental_quests && report_name == o.report_name &&
           report_email == o.report_email;
}

Settings parse_settings(std::string_view text) {
    if (text.substr(0, kMark.size()) == kMark) text.remove_prefix(kMark.size());
    // The last line for a key wins, as it does in the launcher's reading of the file.
    std::map<std::string, std::string> found;
    for (const std::string& raw : lines_of(text)) {
        std::string value;
        const std::string key = key_of(raw, &value);
        if (!key.empty()) found[key] = value;
    }
    Settings s;
    const auto text_of = [&](const char* key) {
        const auto it = found.find(key);
        return it == found.end() ? std::string() : it->second;
    };
    const auto flag = [&](const char* key, bool otherwise) {
        const auto it = found.find(key);
        return it == found.end() ? otherwise : truthy(it->second, otherwise);
    };
    s.root = text_of("root");
    s.args = text_of("args");
    s.mic = flag("mic", s.mic);
    s.launch_game = flag("launch_game", s.launch_game);
    s.autostart = flag("autostart", s.autostart);
    s.api = lowered(text_of("model")) == "api";
    s.api_url = text_of("api_url");
    s.api_model = text_of("api_model");
    s.api_extra = text_of("api_extra");
    s.api_guessing = flag("api_guessing", s.api_guessing);
    s.experimental_quests = flag("experimental_quests", s.experimental_quests);
    s.report_name = text_of("report_name");
    s.report_email = text_of("report_email");
    return s;
}

std::string merge_settings(std::string_view text, const Settings& settings) {
    // The file's own line endings are kept (the installer writes CRLF), and a new file
    // gets the system's.
#ifdef _WIN32
    const bool fresh_crlf = true;
#else
    const bool fresh_crlf = false;
#endif
    const bool crlf = text.empty() ? fresh_crlf : text.find("\r\n") != std::string_view::npos;
    if (text.substr(0, kMark.size()) == kMark) text.remove_prefix(kMark.size());
    std::vector<std::string> lines = lines_of(text);
    for (const Entry& entry : entries_of(settings)) {
        const std::string written = std::string(entry.key) + " = " + entry.value;
        bool placed = false;
        for (auto it = lines.begin(); it != lines.end();) {
            if (key_of(*it) != entry.key) {
                ++it;
                continue;
            }
            // The first line for the key is rewritten where it stands; a default, and any
            // later line for the same key, goes.
            if (!placed && !entry.is_default) {
                *it = written;
                placed = true;
                ++it;
            } else {
                it = lines.erase(it);
            }
        }
        if (!placed && !entry.is_default) lines.push_back(written);
    }
    std::string out;
    for (const std::string& line : lines) out += line + (crlf ? "\r\n" : "\n");
    // Written as UTF-8. The mark at the front is what tells Windows PowerShell, which
    // reads this file for the console launcher and the log collector, that it is; it is
    // only there when something in the file needs it.
    if (!ascii(out)) out.insert(0, kMark);
    return out;
}

Settings SettingsFile::load() const { return parse_settings(read_file(path_)); }

bool SettingsFile::save(const Settings& settings, std::string* trouble) const {
    const std::string after = merge_settings(read_file(path_), settings);
    // Beside the file and then over it, so that a write cut short leaves the old settings
    // and not half of the new.
    std::filesystem::path part = path_;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << after;
        out.flush();
        if (!out) {
            if (trouble) *trouble = "could not write " + part.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(part, path_, ec);
    if (ec) {
        if (trouble) *trouble = "could not replace " + path_.string() + ": " + ec.message();
        std::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

std::vector<std::string> split_args(std::string_view line) {
    std::vector<std::string> out;
    std::string piece;
    bool quoted = false;
    bool any = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (c == '\\' && i + 1 < line.size() && line[i + 1] == '"') {
            piece += '"';
            any = true;
            ++i;
        } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (any) out.push_back(piece);
            piece.clear();
            any = false;
        } else {
            piece += c;
            any = true;
        }
    }
    if (any) out.push_back(piece);
    return out;
}

}  // namespace moonshine::gui
