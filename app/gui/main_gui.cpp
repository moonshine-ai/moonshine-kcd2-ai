// Moonshine: the window.
//
// What a player opens. It shows what the app is doing in a line, starts and stops it with
// one button, and keeps the logs, the settings, updating and removing a click away rather
// than in front of them. It replaces the PowerShell console that installer/Moonshine.ps1
// ran, and does what that did: checks the installation, asks Steam for the game, and
// keeps the engine (moonshine_app, a separate program -- see supervisor.h) running.
//
// This file is only the window: a native frame around a web page, through the webview
// library (WebView2 on Windows, which Windows 11 has and most of Windows 10; WKWebView on
// the Mac, where this is built to be worked on). Everything the window does is in
// bridge.h, which has no window in it and is what the tests drive. Here:
//
//   - one instance: a second start brings the first window forward and ends;
//   - the page, from inside the program or (MOONSHINE_UI_DIR) from a folder;
//   - moonshineCall(method, args) bound for the page, and the state pushed to it;
//   - the close button asking first while the characters are listening;
//   - the browser put to sleep while the window is minimised, which is where it is while
//     the game is being played, on a PC whose memory the game wants;
//   - the old console launcher offered when there is no WebView2 to make a window with.
//
//   --play              press Play as soon as the check has passed, even where the setting
//                       "Press Play for me when Moonshine opens" has been turned off: for
//                       a script that means "start playing" (scripts/start-moonshine-on-wl1.sh)
//   --no-play           do not, even where that setting is on (which it is unless turned
//                       off): the window opened to be looked at. The installer opens it
//                       this way at the end of an install and of an update, and so do the
//                       scripts that press Play themselves
//
//   MOONSHINE_HOME      the installation, when it is not the folder this program is in
//                       (a checkout: the repository's top, with the engine in app/build)
//   MOONSHINE_UI_DIR    read the page from here (app/gui/ui) rather than from the program
//   MOONSHINE_UI_DEBUG  allow the browser's developer tools
//   MOONSHINE_UI_SCRIPT a file of JavaScript to run in the page once it has loaded, which
//                       is how the real window is driven by a script rather than a hand
//                       (scripts/gui-walk.js); what it passes to moonshineCall('trace', {...})
//                       is printed, a line each, to stderr
//   MOONSHINE_UI_TRACE  ... and added to this file, since a Windows program with a
//                       window has no stderr unless it is given one

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "bridge.h"
#include "json.hpp"
#include "page.h"
#include "child.h"
#include "supervisor.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "webview.h"

#ifdef _WIN32
#include <shobjidl.h>
// The look of this decade for the folder picker and the one message box.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace moonshine::gui {

// The page, as bytes in the program (written by embed.cmake).
extern const unsigned char kUiHtml[];
extern const std::size_t kUiHtmlSize;
extern const unsigned char kUiCss[];
extern const std::size_t kUiCssSize;
extern const unsigned char kUiJs[];
extern const std::size_t kUiJsSize;

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

std::string dumped(const json& value) {
    // A log can hold bytes that are not UTF-8 (a path in the system's own encoding); they
    // are shown as the replacement character rather than failing the whole answer.
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string page() {
    const std::string folder = environment("MOONSHINE_UI_DIR");
    if (!folder.empty()) {
        std::string trouble;
        if (const std::optional<std::string> found = page_from(path_from_utf8(folder), &trouble)) return *found;
        return "<body style=\"font:14px system-ui;padding:40px\">MOONSHINE_UI_DIR: " + trouble + "</body>";
    }
    const auto text = [](const unsigned char* bytes, std::size_t size) {
        return std::string_view(reinterpret_cast<const char*>(bytes), size);
    };
    return inline_page(text(kUiHtml, kUiHtmlSize), text(kUiCss, kUiCssSize), text(kUiJs, kUiJsSize));
}

// Where the installation is, and where the engine is: beside this program, unless told.
Paths find_paths() {
    const fs::path here = own_path().parent_path();
    const std::string told = environment("MOONSHINE_HOME");
    const fs::path home = told.empty() ? here : path_from_utf8(told);
    Paths paths = Paths::at(home);
    std::error_code ec;
    if (!fs::exists(paths.engine, ec)) {
        const Paths beside = Paths::at(home, here);
        if (fs::exists(beside.engine, ec)) paths = beside;
    }
    return paths;
}

#ifdef _WIN32

// Two names. The first says a window is open: a second start looks for it, and the
// installer (AppMutex in moonshine.iss) will not replace a program that holds it. It is
// let go just before the installer or the uninstaller is started, so that they do not
// stop to ask for this app to be closed when it is already closing. The second is held
// to the very end, and is what they wait on before touching the files.
constexpr const wchar_t* kWindowMutex = L"Moonshine.KCD2.Window";
constexpr const wchar_t* kProcessMutex = L"Moonshine.KCD2.Process";
constexpr const wchar_t* kTitle = L"Moonshine";

HANDLE g_window_mutex = nullptr;

struct Frame {
    webview::webview* view = nullptr;
    Controller* controller = nullptr;
    WNDPROC inner = nullptr;
    std::atomic<bool> minimized{false};
    bool closing = false;
    std::chrono::steady_clock::time_point asked{};
};
Frame g_frame;

ICoreWebView2* core_of(webview::webview& view, ICoreWebView2Controller** controller_out = nullptr) {
    const auto found = view.browser_controller();
    if (!found.ok() || !found.value()) return nullptr;
    auto* controller = static_cast<ICoreWebView2Controller*>(found.value());
    if (controller_out) *controller_out = controller;
    ICoreWebView2* core = nullptr;
    controller->get_CoreWebView2(&core);
    return core;
}

// A page of our own does not want a browser's furniture: its menus, its zoom, its
// shortcuts for printing and reloading, or its offer to remember what was typed into a
// form -- least of all a form with an API key in it.
void quieten(webview::webview& view, bool debug) {
    ICoreWebView2* core = core_of(view);
    if (!core) return;
    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(core->get_Settings(&settings)) && settings) {
        if (!debug) settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        ICoreWebView2Settings3* third = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings3), reinterpret_cast<void**>(&third)))) {
            if (!debug) third->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            third->Release();
        }
        ICoreWebView2Settings4* fourth = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings4), reinterpret_cast<void**>(&fourth)))) {
            fourth->put_IsPasswordAutosaveEnabled(FALSE);
            fourth->put_IsGeneralAutofillEnabled(FALSE);
            fourth->Release();
        }
        ICoreWebView2Settings5* fifth = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings5), reinterpret_cast<void**>(&fifth)))) {
            fifth->put_IsPinchZoomEnabled(FALSE);
            fifth->Release();
        }
        settings->Release();
    }
    core->Release();
}

// Minimised is where this window spends the evening, beside a game that wants the
// memory. The browser is told it is not being looked at, asked to give back what it can,
// and suspended (its scripts stop; nothing here needs them while nobody is looking -- the
// engine is kept by the supervisor's own thread).
void rest(webview::webview& view, bool resting) {
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* core = core_of(view, &controller);
    if (!core) return;
    ICoreWebView2_19* memory = nullptr;
    if (SUCCEEDED(core->QueryInterface(__uuidof(ICoreWebView2_19), reinterpret_cast<void**>(&memory)))) {
        memory->put_MemoryUsageTargetLevel(resting ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
                                                   : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        memory->Release();
    }
    ICoreWebView2_3* sleeper = nullptr;
    const bool can_sleep = SUCCEEDED(core->QueryInterface(__uuidof(ICoreWebView2_3), reinterpret_cast<void**>(&sleeper)));
    if (resting) {
        // It has to be invisible before it will suspend.
        controller->put_IsVisible(FALSE);
        if (can_sleep) sleeper->TrySuspend(nullptr);
    } else {
        if (can_sleep) sleeper->Resume();
        controller->put_IsVisible(TRUE);
    }
    if (can_sleep) sleeper->Release();
    core->Release();
}

LRESULT CALLBACK frame_proc(HWND window, UINT message, WPARAM wide, LPARAM low) {
    Frame& frame = g_frame;
    if (message == WM_CLOSE && !frame.closing && frame.controller && frame.controller->engine_active()) {
        // The characters are listening: closing this stops them, so it is asked first, in
        // the page. Asked twice within a few seconds it is simply done -- a page that could
        // not put the question must not make the window impossible to close.
        const auto now = std::chrono::steady_clock::now();
        if (now - frame.asked > std::chrono::seconds(5)) {
            frame.asked = now;
            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
            frame.view->eval("window.moonshine && window.moonshine.confirmClose()");
            return 0;
        }
    }
    if (message == WM_SIZE) {
        const bool minimized = wide == SIZE_MINIMIZED;
        if (minimized != frame.minimized.load()) {
            frame.minimized = minimized;
            rest(*frame.view, minimized);
        }
    }
    return CallWindowProcW(frame.inner, window, message, wide, low);
}

std::wstring widened(const std::string& text) { return path_from_utf8(text).wstring(); }

std::string pick_folder(HWND owner, const std::string& from) {
    std::string picked;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return picked;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose the folder Kingdom Come: Deliverance 2 is installed in");
    if (!from.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(widened(from).c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dialog->SetFolder(start);
            start->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                picked = utf8_of(fs::path(path));
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return picked;
}

// No WebView2, so no window. The console launcher that came before this one is still in
// the installation, and does the same job in a console.
int offer_console(const Paths& paths, const std::string& why) {
    const fs::path script = paths.home / "Moonshine.ps1";
    std::error_code ec;
    const bool have = fs::exists(script, ec);
    std::wstring text = L"Moonshine's window could not be opened (" + widened(why) +
                        L").\n\nIt needs Microsoft Edge WebView2, which Windows 11 includes and the Moonshine "
                        L"installer adds when it is missing. Running the installer again will put it back.";
    if (have) text += L"\n\nStart Moonshine in a console window instead?";
    const int answer = MessageBoxW(nullptr, text.c_str(), kTitle, (have ? MB_YESNO : MB_OK) | MB_ICONWARNING);
    if (have && answer == IDYES) {
        std::string root = environment("SystemRoot");
        if (root.empty()) root = "C:\\Windows";
        Command command;
        command.exe = path_from_utf8(root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe";
        command.cwd = paths.home;
        command.args = {"-NoExit", "-ExecutionPolicy", "Bypass", "-File", utf8_of(script)};
        launch(command);
    }
    return 1;
}

#else

std::string pick_folder(void*, const std::string&) {
    // The Mac build is for working on the window; the system's own chooser, through the
    // scripting tool that comes with it, is enough.
    Command command;
    command.exe = "/usr/bin/osascript";
    command.args = {"-e", "POSIX path of (choose folder with prompt \"Choose the game's folder\")"};
    const Ran ran = run(command, std::chrono::minutes(10));
    std::string path = ran.finished && ran.code == 0 ? ran.output : std::string();
    while (!path.empty() && (path.back() == '\n' || path.back() == '/')) path.pop_back();
    return path;
}

#endif

int run_window(Controller::Start how) {
    const Paths paths = find_paths();
    const bool debug = !environment("MOONSHINE_UI_DEBUG").empty();

#ifdef _WIN32
    g_window_mutex = CreateMutexW(nullptr, FALSE, kWindowMutex);
    if (g_window_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // One is open already: bring it forward, and that is all this start does.
        if (const HWND other = FindWindowW(L"webview", kTitle)) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    CreateMutexW(nullptr, FALSE, kProcessMutex);
    // The browser's own files (a cache, mostly) go beside the app, where the uninstaller
    // removes them, not under the roaming profile where the library would put them.
    std::error_code made;
    fs::create_directories(paths.home / "webview", made);
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", (paths.home / "webview").wstring().c_str());
#endif

    std::unique_ptr<webview::webview> view;
    try {
        view = std::make_unique<webview::webview>(debug, nullptr);
    } catch (const std::exception& error) {
#ifdef _WIN32
        return offer_console(paths, error.what());
#else
        std::fprintf(stderr, "Moonshine: no window: %s\n", error.what());
        return 1;
#endif
    }
    webview::webview& w = *view;
    w.set_title("Moonshine");
    w.set_size(900, 640, WEBVIEW_HINT_NONE);
    w.set_size(720, 540, WEBVIEW_HINT_MIN);

    Controller::Parts parts = Controller::Parts::system(paths);
    parts.quit = [&w] {
        w.dispatch([&w] {
#ifdef _WIN32
            g_frame.closing = true;
#endif
            w.terminate();
        });
    };
    parts.release_instance = [] {
#ifdef _WIN32
        if (g_window_mutex) CloseHandle(g_window_mutex);
        g_window_mutex = nullptr;
#endif
    };
    Controller controller(std::move(parts));

#ifdef _WIN32
    const HWND window = static_cast<HWND>(w.window().value());
    g_frame.view = &w;
    g_frame.controller = &controller;
    g_frame.inner = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(frame_proc)));
    quieten(w, debug);
#else
    void* window = nullptr;
#endif

    // The one way in from the page: a method's name and its arguments. Answered at once
    // from the thread the window draws on, except for the few that take time.
    w.bind(
        "moonshineCall",
        [&](const std::string& id, const std::string& request, void*) {
            const json params = json::parse(request, nullptr, false);
            const std::string method = params.is_array() && !params.empty() && params[0].is_string()
                                           ? params[0].get<std::string>()
                                           : std::string();
            const json args = params.is_array() && params.size() > 1 && params[1].is_object() ? params[1] : json::object();
            if (method == "trace") {
                // Only a script that drives the window calls this (MOONSHINE_UI_SCRIPT).
                const std::string line = dumped(args);
                std::fprintf(stderr, "%s\n", line.c_str());
                std::fflush(stderr);
                if (const std::string file = environment("MOONSHINE_UI_TRACE"); !file.empty()) {
                    std::ofstream(path_from_utf8(file), std::ios::binary | std::ios::app) << line << "\n";
                }
                w.resolve(id, 0, "{\"ok\":true}");
            } else if (method == "quit") {
                w.resolve(id, 0, "{\"ok\":true}");
#ifdef _WIN32
                g_frame.closing = true;
#endif
                w.terminate();
            } else if (method == "chooseFolder") {
                // Not from inside the browser's own callback, which a modal box must not
                // be opened in: once this has returned.
                w.dispatch([&w, &controller, window, id] {
                    const std::string path = pick_folder(window, controller.game_root());
                    w.resolve(id, 0, dumped({{"ok", !path.empty()}, {"path", path}}));
                });
            } else if (Controller::slow(method)) {
                controller.in_background([&w, &controller, id, method, args] {
                    w.resolve(id, 0, dumped(controller.call(method, args)));
                });
            } else {
                w.resolve(id, 0, dumped(controller.call(method, args)));
            }
        },
        nullptr);

    if (const std::string script = environment("MOONSHINE_UI_SCRIPT"); !script.empty()) {
        std::ifstream in(path_from_utf8(script), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        w.init("window.addEventListener('load', function () {\n" + text.str() + "\n});");
    }
    w.set_html(page());
    controller.start(how);

    // The state, sent to the page whenever it differs from what was last sent. Not while
    // the window is minimised: the page is asleep then, and is told on waking.
    std::atomic<bool> done{false};
    std::thread pusher([&] {
        std::string sent;
        while (!done) {
            controller.wait_for_change(std::chrono::milliseconds(400));
            if (done) break;
#ifdef _WIN32
            if (g_frame.minimized) {
                sent.clear();
                continue;
            }
#endif
            std::string now = dumped(controller.state());
            if (now == sent) continue;
            sent = now;
            w.dispatch([&w, now = std::move(now)] { w.eval("window.moonshine && window.moonshine.push(" + now + ")"); });
        }
    });

    w.run();

    done = true;
    pusher.join();
#ifdef _WIN32
    // Stopping the engine can take a few seconds; the window does not stand there dead
    // while it does.
    if (IsWindow(window)) ShowWindow(window, SW_HIDE);
#endif
    // The engine is told to go, and gone, before the window's process is.
    controller.shutdown();
#ifdef _WIN32
    // What was still on its way to the page -- an answer from a thread that has only now
    // finished, the last state -- is delivered while there is still a page. Left queued,
    // the library runs it from its destructor after letting go of the browser, and falls
    // over (the update did: it closes the window from a worker, with the page mid-call).
    // Nothing adds to the queue any more: the pusher and the workers have been joined.
    MSG pending;
    while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&pending);
        DispatchMessageW(&pending);
    }
    g_frame.view = nullptr;
    g_frame.controller = nullptr;
#endif
    return 0;
}

}  // namespace
}  // namespace moonshine::gui

#ifdef _WIN32
// --play and --no-play: press Play as soon as the check passes, or do not, whatever the
// setting says. With neither it is as the setting says, which is to press it.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    using Start = moonshine::gui::Controller::Start;
    Start how = Start::as_set;
    if (arguments && std::wcsstr(arguments, L"--no-play") != nullptr) how = Start::wait;
    else if (arguments && std::wcsstr(arguments, L"--play") != nullptr) how = Start::play;
    return moonshine::gui::run_window(how);
}
#else
int main(int argc, char** argv) {
    using Start = moonshine::gui::Controller::Start;
    Start how = Start::as_set;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-play") how = Start::wait;
        else if (std::string(argv[i]) == "--play" && how != Start::wait) how = Start::play;
    }
    return moonshine::gui::run_window(how);
}
#endif
