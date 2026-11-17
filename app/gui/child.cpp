#include "child.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>
#include <wchar.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace moonshine::gui {

namespace {

// What is kept of a program that goes on printing: the engine says everything to its log
// as well, so this is only ever read for the last words of one that died early.
constexpr std::size_t kKeptOutput = 256 * 1024;

// The printed output of a child, filled by the thread that reads its pipe.
class Output {
public:
    void add(const char* bytes, std::size_t count) {
        std::lock_guard<std::mutex> held(lock_);
        text_.append(bytes, count);
        if (text_.size() > kKeptOutput * 2) text_.erase(0, text_.size() - kKeptOutput);
    }
    std::string take() {
        std::lock_guard<std::mutex> held(lock_);
        std::string out;
        out.swap(text_);
        return out;
    }
    void done() {
        {
            std::lock_guard<std::mutex> held(lock_);
            done_ = true;
        }
        finished_.notify_all();
    }
    bool wait_done(std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> held(lock_);
        return finished_.wait_for(held, limit, [this] { return done_; });
    }

private:
    std::mutex lock_;
    std::condition_variable finished_;
    std::string text_;
    bool done_ = false;
};

}  // namespace

std::string quote_argument(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(argument);
    }
    // The rule the C runtime reads a command line back by: backslashes are themselves
    // except before a quote, where each is doubled and the quote escaped; so a path that
    // ends in a backslash does not swallow the closing quote.
    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
        } else {
            out.append(slashes, '\\');
        }
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');
    out += '"';
    return out;
}

std::string command_line(const Command& command) {
    std::string line = quote_argument(utf8_of(command.exe));
    for (const std::string& arg : command.args) line += " " + quote_argument(arg);
    return line;
}

Ran run(const Command& command, std::chrono::milliseconds limit, const std::function<bool()>& cancelled) {
    Ran ran;
    std::unique_ptr<Child> child = spawn(command, &ran.trouble);
    if (!child) return ran;
    ran.started = true;
    // Nothing is going to be typed at it.
    child->hang_up();
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (true) {
        if (const std::optional<int> code = child->wait_for(std::chrono::milliseconds(50))) {
            ran.finished = true;
            ran.code = *code;
            break;
        }
        ran.output += child->take_output();
        if (std::chrono::steady_clock::now() >= deadline) {
            ran.trouble = "it did not finish in time";
            break;
        }
        if (cancelled && cancelled()) {
            ran.trouble = "stopped";
            break;
        }
    }
    if (!ran.finished) child->kill();
    ran.output += child->take_output();
    return ran;
}

#ifdef _WIN32

namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::string last_error(const char* what) {
    const DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string said = text ? narrow(text) : std::string();
    if (text) LocalFree(text);
    while (!said.empty() && (said.back() == '\n' || said.back() == '\r' || said.back() == ' ')) said.pop_back();
    return std::string(what) + ": " + (said.empty() ? "error " + std::to_string(code) : said);
}

// Names in an environment are not told apart by case, and the block is kept in order.
struct NameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& extra) {
    std::map<std::wstring, std::wstring, NameLess> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* at = block; *at; at += wcslen(at) + 1) {
            const std::wstring entry(at);
            // The drive-current-directory entries begin with '=' ("=C:=C:\here"); the name
            // ends at the first '=' after the first character.
            const std::size_t equals = entry.find(L'=', 1);
            if (equals == std::wstring::npos) continue;
            vars[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : extra) vars[widen(name)] = widen(value);
    std::wstring out;
    for (const auto& [name, value] : vars) {
        out += name;
        out += L'=';
        out += value;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

class WinChild : public Child {
public:
    WinChild(HANDLE process, HANDLE job, HANDLE keyboard, HANDLE printed)
        : process_(process), job_(job), keyboard_(keyboard), printed_(printed) {
        reader_ = std::thread([this] {
            char buffer[4096];
            DWORD got = 0;
            while (ReadFile(printed_, buffer, sizeof buffer, &got, nullptr) && got > 0) {
                output_.add(buffer, got);
            }
            output_.done();
        });
    }

    ~WinChild() override {
        kill();
        // Closing the job takes anything the program started with it, which is also what
        // lets go of the far end of the pipe the reader is blocked on.
        if (job_) CloseHandle(job_);
        if (!output_.wait_done(std::chrono::seconds(2))) CancelIoEx(printed_, nullptr);
        if (reader_.joinable()) reader_.join();
        hang_up();
        CloseHandle(printed_);
        CloseHandle(process_);
    }

    std::optional<int> wait_for(std::chrono::milliseconds limit) override {
        if (code_) return code_;
        if (WaitForSingleObject(process_, static_cast<DWORD>(limit.count())) != WAIT_OBJECT_0) return std::nullopt;
        DWORD code = 1;
        GetExitCodeProcess(process_, &code);
        // Its last words are in the pipe a moment after it is gone.
        output_.wait_done(std::chrono::seconds(2));
        code_ = static_cast<int>(code);
        return code_;
    }

    bool say(std::string_view line) override {
        std::lock_guard<std::mutex> held(keyboard_lock_);
        if (!keyboard_) return false;
        std::string text(line);
        text += "\n";
        DWORD wrote = 0;
        return WriteFile(keyboard_, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) &&
               wrote == text.size();
    }

    void hang_up() override {
        std::lock_guard<std::mutex> held(keyboard_lock_);
        if (keyboard_) CloseHandle(keyboard_);
        keyboard_ = nullptr;
    }

    void kill() override {
        if (code_) return;
        if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) return;
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 5000);
    }

    std::string take_output() override { return output_.take(); }

private:
    HANDLE process_;
    HANDLE job_;
    HANDLE keyboard_;
    HANDLE printed_;
    std::mutex keyboard_lock_;
    std::thread reader_;
    Output output_;
    std::optional<int> code_;
};

}  // namespace

std::filesystem::path path_from_utf8(std::string_view text) { return std::filesystem::path(widen(text)); }
std::string utf8_of(const std::filesystem::path& path) { return narrow(path.wstring()); }

std::filesystem::path own_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD got = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (got == 0) return {};
        if (got < buffer.size()) {
            buffer.resize(got);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::string environment(const char* name) {
    const std::wstring wide = widen(name);
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD got = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    value.resize(got);
    return narrow(value);
}

std::unique_ptr<Child> spawn(const Command& command, std::string* trouble) {
    const auto fail = [&](const char* what) {
        if (trouble) *trouble = last_error(what);
        return nullptr;
    };

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof inherit;
    inherit.bInheritHandle = TRUE;

    // Two pipes: its keyboard and what it prints. Our ends are not for it to inherit.
    HANDLE keyboard_read = nullptr, keyboard_write = nullptr, printed_read = nullptr, printed_write = nullptr;
    if (!CreatePipe(&keyboard_read, &keyboard_write, &inherit, 0)) return fail("could not make a pipe");
    if (!CreatePipe(&printed_read, &printed_write, &inherit, 0)) {
        CloseHandle(keyboard_read);
        CloseHandle(keyboard_write);
        return fail("could not make a pipe");
    }
    SetHandleInformation(keyboard_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(printed_read, HANDLE_FLAG_INHERIT, 0);
    const auto close_pipes = [&] {
        for (HANDLE handle : {keyboard_read, keyboard_write, printed_read, printed_write}) CloseHandle(handle);
    };

    // Only those two ends go to it, whatever else in this process happens to be
    // inheritable (the webview opens handles of its own); a pipe end that leaked into
    // some other child would keep this one's keyboard open after we had closed it.
    HANDLE passed[] = {keyboard_read, printed_write};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, passed, sizeof passed, nullptr,
                                   nullptr)) {
        close_pipes();
        return fail("could not prepare the start");
    }

    STARTUPINFOEXW start{};
    start.StartupInfo.cb = sizeof start;
    start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    start.StartupInfo.hStdInput = keyboard_read;
    start.StartupInfo.hStdOutput = printed_write;
    start.StartupInfo.hStdError = printed_write;
    start.lpAttributeList = list;

    std::wstring line = widen(command_line(command));
    std::wstring block = environment_block(command.env);
    const std::wstring cwd = command.cwd.empty() ? std::wstring() : command.cwd.wstring();
    PROCESS_INFORMATION made{};
    // Suspended until it is in its job, so that nothing it starts can be outside it.
    const BOOL ok = CreateProcessW(
        command.exe.wstring().c_str(), line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
        block.data(), cwd.empty() ? nullptr : cwd.c_str(), &start.StartupInfo, &made);
    const DWORD why = GetLastError();
    DeleteProcThreadAttributeList(list);
    CloseHandle(keyboard_read);
    CloseHandle(printed_write);
    if (!ok) {
        CloseHandle(keyboard_write);
        CloseHandle(printed_read);
        SetLastError(why);
        if (trouble) *trouble = last_error(("could not start " + utf8_of(command.exe.filename())).c_str());
        return nullptr;
    }

    // If we are killed, it goes too: the last handle to the job closes with us.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
            !AssignProcessToJobObject(job, made.hProcess)) {
            // Not fatal: --quit-on-eof stops it when our end of its keyboard closes.
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(made.hThread);
    CloseHandle(made.hThread);
    return std::make_unique<WinChild>(made.hProcess, job, keyboard_write, printed_read);
}

bool launch(const Command& command, std::string* trouble) {
    std::string parameters;
    for (const std::string& arg : command.args) {
        if (!parameters.empty()) parameters += " ";
        parameters += quote_argument(arg);
    }
    const std::wstring file = command.exe.wstring();
    const std::wstring wide = widen(parameters);
    const std::wstring cwd = command.cwd.wstring();
    // Through the shell, as Start-Process did it: it is the shell that raises the consent
    // prompt for a program that asks for one, where CreateProcess only fails.
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.lpParameters = wide.empty() ? nullptr : wide.c_str();
    info.lpDirectory = cwd.empty() ? nullptr : cwd.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        if (trouble) *trouble = last_error(("could not start " + utf8_of(command.exe.filename())).c_str());
        return false;
    }
    return true;
}

bool open_target(const std::string& what) {
    const std::wstring wide = widen(what);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) >
           32;
}

namespace {

// Every running process, as (pid, file name).
std::vector<std::pair<DWORD, std::wstring>> processes() {
    std::vector<std::pair<DWORD, std::wstring>> out;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            out.emplace_back(entry.th32ProcessID, entry.szExeFile);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return out;
}

bool same_name(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

bool program_running(std::string_view image_name) {
    const std::wstring wanted = widen(image_name);
    for (const auto& [pid, name] : processes()) {
        if (same_name(name, wanted)) return true;
    }
    return false;
}

std::vector<long> running_copies(const std::filesystem::path& exe) {
    std::vector<long> out;
    std::error_code ec;
    const std::filesystem::path wanted = std::filesystem::weakly_canonical(exe, ec);
    const std::wstring name = exe.filename().wstring();
    for (const auto& [pid, image] : processes()) {
        if (!same_name(image, name)) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        const bool known = QueryFullProcessImageNameW(process, 0, path.data(), &length) != 0;
        // One that has exited and is only still listed is not a copy running.
        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        if (!known || !alive) continue;
        path.resize(length);
        const std::filesystem::path found = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
        if (same_name(found.wstring(), wanted.wstring())) out.push_back(static_cast<long>(pid));
    }
    return out;
}

bool kill_process(long pid) {
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    const bool ok = TerminateProcess(process, 1) != 0;
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
    return ok;
}

std::filesystem::path installed_uninstaller(const std::filesystem::path& home) {
    // Where Inno Setup records an installation, under the AppId in installer/moonshine.iss:
    // for this user, or for the machine when it was installed for everyone.
    static const wchar_t* kKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{7C2E3C1A-5B7D-4E7E-9C64-3A1B2C4D5E6F}_is1";
    std::error_code ec;
    for (const HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t buffer[2048];
        DWORD size = sizeof buffer;
        if (RegGetValueW(hive, kKey, L"UninstallString", RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
            continue;
        }
        std::wstring text(buffer);
        text.erase(std::remove(text.begin(), text.end(), L'"'), text.end());
        const std::filesystem::path exe(text);
        // Only the one for this folder: a build run from somewhere else must not offer to
        // remove the installed copy.
        if (std::filesystem::exists(exe, ec) && std::filesystem::equivalent(exe.parent_path(), home, ec)) return exe;
    }
    // Not recorded under that name (a tester build has its own): the one in the folder.
    // Inno Setup numbers them, and takes the next number when the first is in the way, so
    // it is the newest that still has its record of what was installed beside it.
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(home, ec)) {
        const std::filesystem::path file = entry.path();
        const std::wstring name = file.filename().wstring();
        if (name.size() != 12 || _wcsnicmp(name.c_str(), L"unins", 5) != 0 || _wcsicmp(name.c_str() + 8, L".exe") != 0) {
            continue;
        }
        if (!std::all_of(name.begin() + 5, name.begin() + 8, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
        std::filesystem::path record = file;
        record.replace_extension(L".dat");
        if (!std::filesystem::exists(record, ec)) continue;
        if (found.empty() || _wcsicmp(name.c_str(), found.filename().c_str()) > 0) found = file;
    }
    return found;
}

#else  // POSIX: the Mac and Linux, where the window is built for working on it.

namespace {

class PosixChild : public Child {
public:
    PosixChild(pid_t pid, int keyboard, int printed) : pid_(pid), keyboard_(keyboard), printed_(printed) {
        reader_ = std::thread([this] {
            char buffer[4096];
            while (!stop_) {
                pollfd waiting{printed_, POLLIN, 0};
                const int ready = ::poll(&waiting, 1, 100);
                if (ready < 0 && errno != EINTR) break;
                if (ready <= 0) continue;
                const ssize_t got = ::read(printed_, buffer, sizeof buffer);
                if (got > 0) {
                    output_.add(buffer, static_cast<std::size_t>(got));
                } else if (got == 0 || errno != EINTR) {
                    break;
                }
            }
            output_.done();
        });
    }

    ~PosixChild() override {
        kill();
        stop_ = true;
        if (reader_.joinable()) reader_.join();
        hang_up();
        ::close(printed_);
    }

    std::optional<int> wait_for(std::chrono::milliseconds limit) override {
        if (code_) return code_;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (true) {
            int status = 0;
            const pid_t got = ::waitpid(pid_, &status, WNOHANG);
            if (got == pid_) {
                code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                output_.wait_done(std::chrono::seconds(2));
                return code_;
            }
            if (got < 0 && errno != EINTR) {
                code_ = 1;
                return code_;
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool say(std::string_view line) override {
        std::lock_guard<std::mutex> held(keyboard_lock_);
        if (keyboard_ < 0) return false;
        std::string text(line);
        text += "\n";
        std::size_t done = 0;
        while (done < text.size()) {
            const ssize_t wrote = ::write(keyboard_, text.data() + done, text.size() - done);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) return false;
            done += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    void hang_up() override {
        std::lock_guard<std::mutex> held(keyboard_lock_);
        if (keyboard_ >= 0) ::close(keyboard_);
        keyboard_ = -1;
    }

    void kill() override {
        if (code_) return;
        ::kill(pid_, SIGKILL);
        wait_for(std::chrono::seconds(5));
    }

    std::string take_output() override { return output_.take(); }

private:
    pid_t pid_;
    int keyboard_;
    int printed_;
    std::mutex keyboard_lock_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    Output output_;
    std::optional<int> code_;
};

std::vector<std::string> environment_list(const std::vector<std::pair<std::string, std::string>>& extra) {
    std::map<std::string, std::string> vars;
    for (char** at = environ; at && *at; ++at) {
        const std::string entry(*at);
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos) continue;
        vars[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
    for (const auto& [name, value] : extra) vars[name] = value;
    std::vector<std::string> out;
    for (const auto& [name, value] : vars) out.push_back(name + "=" + value);
    return out;
}

std::vector<char*> pointers(std::vector<std::string>& strings) {
    std::vector<char*> out;
    for (std::string& s : strings) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

}  // namespace

std::filesystem::path path_from_utf8(std::string_view text) { return std::filesystem::path(std::string(text)); }
std::string utf8_of(const std::filesystem::path& path) { return path.string(); }

std::filesystem::path own_path() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code ec;
    const std::filesystem::path found = std::filesystem::canonical(buffer.c_str(), ec);
    return ec ? std::filesystem::path(buffer.c_str()) : found;
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

std::unique_ptr<Child> spawn(const Command& command, std::string* trouble) {
    const auto fail = [&](const std::string& what) {
        if (trouble) *trouble = what;
        return nullptr;
    };
    int keyboard[2] = {-1, -1};
    int printed[2] = {-1, -1};
    if (::pipe(keyboard) != 0) return fail(std::string("could not make a pipe: ") + std::strerror(errno));
    if (::pipe(printed) != 0) {
        ::close(keyboard[0]);
        ::close(keyboard[1]);
        return fail(std::string("could not make a pipe: ") + std::strerror(errno));
    }
    // Ours are closed in anything started from here on; the two the child is given are
    // put in place by the file actions, which leaves them open in it.
    for (const int fd : {keyboard[0], keyboard[1], printed[0], printed[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, keyboard[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, printed[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, printed[1], STDERR_FILENO);
    const std::string cwd = command.cwd.string();
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());

    std::vector<std::string> args;
    args.push_back(command.exe.string());
    for (const std::string& arg : command.args) args.push_back(arg);
    std::vector<std::string> env = environment_list(command.env);
    std::vector<char*> argv = pointers(args);
    std::vector<char*> envp = pointers(env);

    pid_t pid = -1;
    const int failed = ::posix_spawn(&pid, command.exe.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(keyboard[0]);
    ::close(printed[1]);
    if (failed != 0) {
        ::close(keyboard[1]);
        ::close(printed[0]);
        return fail("could not start " + command.exe.filename().string() + ": " + std::strerror(failed));
    }
    // A child that has stopped reading must not take us down with it.
    ::signal(SIGPIPE, SIG_IGN);
    return std::make_unique<PosixChild>(pid, keyboard[1], printed[0]);
}

bool launch(const Command& command, std::string* trouble) {
    std::vector<std::string> args;
    args.push_back(command.exe.string());
    for (const std::string& arg : command.args) args.push_back(arg);
    std::vector<std::string> env = environment_list(command.env);
    std::vector<char*> argv = pointers(args);
    std::vector<char*> envp = pointers(env);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const std::string cwd = command.cwd.string();
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    pid_t pid = -1;
    const int failed = ::posix_spawn(&pid, command.exe.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (failed != 0) {
        if (trouble) *trouble = "could not start " + command.exe.filename().string() + ": " + std::strerror(failed);
        return false;
    }
    // Left to run; reaped by a thread so that it does not stay behind as a husk.
    std::thread([pid] {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }).detach();
    return true;
}

bool open_target(const std::string& what) {
    Command command;
#ifdef __APPLE__
    command.exe = "/usr/bin/open";
#else
    command.exe = "/usr/bin/xdg-open";
#endif
    command.args = {what};
    return launch(command);
}

// Neither is asked on these systems: the game is a Windows program, and a second engine
// here is a developer's own business.
bool program_running(std::string_view) { return false; }
std::vector<long> running_copies(const std::filesystem::path&) { return {}; }
bool kill_process(long pid) { return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0; }
std::filesystem::path installed_uninstaller(const std::filesystem::path&) { return {}; }

#endif

}  // namespace moonshine::gui
