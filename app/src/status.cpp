#include "status.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include "json.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace moonshine {

namespace {

long process_id() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

}  // namespace

void CheckReport::ok(std::string_view name, std::string text) {
    items.push_back({std::string(name), std::move(text), "ok"});
}

void CheckReport::missing(std::string_view name, std::string text) {
    items.push_back({std::string(name), std::move(text), "missing"});
}

void CheckReport::optional(std::string_view name, std::string text) {
    items.push_back({std::string(name), std::move(text), "optional"});
}

void CheckReport::note(std::string text) {
    items.push_back({std::string(), std::move(text), "note"});
}

bool CheckReport::ready() const {
    for (const CheckItem& item : items) {
        if (item.state == "missing") return false;
    }
    return true;
}

std::string CheckReport::text() const {
    std::ostringstream out;
    for (const CheckItem& item : items) {
        if (item.name.empty()) {
            out << item.text << "\n";
            continue;
        }
        out << std::left << std::setw(11) << item.name + ":" << item.text
            << (item.state == "missing" ? " -- MISSING" : "") << "\n";
    }
    out << (ready() ? "ready" : "NOT READY: something required is missing, see above") << "\n";
    return out.str();
}

std::string CheckReport::json() const {
    nlohmann::json list = nlohmann::json::array();
    for (const CheckItem& item : items) {
        if (item.name.empty()) continue;
        list.push_back({{"name", item.name}, {"text", item.text}, {"state", item.state}});
    }
    const nlohmann::json whole = {
        {"ready", ready()},
        {"game_root", game_root},
        {"items", std::move(list)},
    };
    // Whatever a path holds, the answer is still JSON: a byte that is not UTF-8 (a folder
    // named in the machine's own code page) is replaced rather than thrown on.
    return whole.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

std::string Status::json() const {
    const nlohmann::json whole = {
        {"phase", phase},
        {"talking_to", talking_to},
        {"model", model},
        {"remote_model", remote_model},
        {"voice", voice},
        {"ear", ear},
        {"game_log", game_log},
        {"error", error},
        {"pid", process_id()},
        {"updated", std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()},
    };
    return whole.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

bool Status::operator==(const Status& other) const {
    return phase == other.phase && talking_to == other.talking_to && model == other.model &&
           remote_model == other.remote_model && voice == other.voice && ear == other.ear &&
           game_log == other.game_log && error == other.error;
}

StatusFile::StatusFile(std::filesystem::path path) : path_(std::move(path)) {}

void StatusFile::update(const std::function<void(Status&)>& change) {
    std::lock_guard<std::mutex> held(lock_);
    Status next = status_;
    change(next);
    if (written_ && next == status_) return;
    status_ = next;
    if (path_.empty()) return;
    write(status_);
    written_ = true;
}

Status StatusFile::now() const {
    std::lock_guard<std::mutex> held(lock_);
    return status_;
}

void StatusFile::write(const Status& status) {
    std::filesystem::path part = path_;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << status.json();
    }
    // A reader with the file open at this instant makes the rename fail on Windows; the
    // snapshot is small and the reader quick, so a few tries a moment apart get it there.
    // One that never lands is left: the next change writes again.
    std::error_code failed;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::filesystem::rename(part, path_, failed);
        if (!failed) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::filesystem::remove(part, failed);
}

}  // namespace moonshine
