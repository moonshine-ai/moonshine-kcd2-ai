# Put the interface's three files into a C++ source as bytes, so that Moonshine.exe carries
# its own page and an installation has no loose HTML to go missing.
#
#   cmake -DUI_DIR=<app/gui/ui> -DOUT=<build>/ui_assets.cpp -P embed.cmake
#
# Arrays of bytes rather than string literals: MSVC stops a literal at 16 KB, and the
# script is longer than that. gui/page.cpp joins the three into the page.
set(text "// Written by app/gui/embed.cmake from app/gui/ui; do not edit.\n#include <cstddef>\n\nnamespace moonshine::gui {\n\n")
set(files index.html app.css app.js)
set(names kUiHtml kUiCss kUiJs)
foreach(index RANGE 2)
    list(GET files ${index} file)
    list(GET names ${index} name)
    file(READ "${UI_DIR}/${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    # Sixteen to a line; a compiler is happier with lines than with one of 300,000 columns.
    string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
    string(APPEND text "extern const unsigned char ${name}[] = {\n${bytes}0x00};\n")
    string(APPEND text "extern const std::size_t ${name}Size = sizeof(${name}) - 1;\n\n")
endforeach()
string(APPEND text "}  // namespace moonshine::gui\n")
file(WRITE "${OUT}" "${text}")
