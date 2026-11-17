// The interface as the one string the webview is given.
//
// It is three files while it is being worked on (ui/index.html, app.css, app.js), because
// that is what an editor and a browser want, and one page inside the program, because a
// page handed over as a string has nowhere to load a stylesheet or a script from. The
// build puts the three files into the program as bytes (embed.cmake); this is what joins
// them. With MOONSHINE_UI_DIR set the files are read from that folder instead, each time
// the window opens, so the interface can be changed without building anything.

#ifndef MOONSHINE_GUI_PAGE_H
#define MOONSHINE_GUI_PAGE_H

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace moonshine::gui {

// index.html with the stylesheet and the script it names put inside it, and the lines
// marked data-dev (the browser-only mock) taken out.
std::string inline_page(std::string_view html, std::string_view css, std::string_view js);

// The same from a folder holding the three files; nothing, with the reason, when one is
// not there.
std::optional<std::string> page_from(const std::filesystem::path& folder, std::string* trouble = nullptr);

}  // namespace moonshine::gui

#endif  // MOONSHINE_GUI_PAGE_H
