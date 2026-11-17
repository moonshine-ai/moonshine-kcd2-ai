#include "page.h"

#include <fstream>
#include <sstream>

#include "child.h"

namespace moonshine::gui {

std::string inline_page(std::string_view html, std::string_view css, std::string_view js) {
    // A script ends at the first "</script" in it, wherever that is, even inside a string.
    std::string script(js);
    for (std::size_t at = script.find("</script"); at != std::string::npos; at = script.find("</script", at + 3)) {
        script.replace(at, 2, "<\\/");
    }
    std::string out;
    out.reserve(html.size() + css.size() + script.size() + 64);
    std::size_t from = 0;
    while (from < html.size()) {
        std::size_t end = html.find('\n', from);
        if (end == std::string_view::npos) end = html.size();
        const std::string_view line = html.substr(from, end - from);
        from = end + 1;
        if (line.find("data-dev") != std::string_view::npos) continue;
        if (line.find("href=\"app.css\"") != std::string_view::npos) {
            out += "<style>\n";
            out += css;
            out += "\n</style>\n";
        } else if (line.find("src=\"app.js\"") != std::string_view::npos) {
            out += "<script>\n";
            out += script;
            out += "\n</script>\n";
        } else {
            out += line;
            out += '\n';
        }
    }
    return out;
}

std::optional<std::string> page_from(const std::filesystem::path& folder, std::string* trouble) {
    std::string parts[3];
    const char* names[3] = {"index.html", "app.css", "app.js"};
    for (int i = 0; i < 3; ++i) {
        std::ifstream in(folder / names[i], std::ios::binary);
        if (!in) {
            if (trouble) *trouble = "no " + std::string(names[i]) + " in " + utf8_of(folder);
            return std::nullopt;
        }
        std::ostringstream text;
        text << in.rdbuf();
        parts[i] = text.str();
    }
    return inline_page(parts[0], parts[1], parts[2]);
}

}  // namespace moonshine::gui
