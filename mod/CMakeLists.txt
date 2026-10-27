cmake_minimum_required(VERSION 3.20)
project(kcd2modgen CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SQLite and miniz are vendored so a checkout builds with nothing installed. SQLite is the
# one amalgamation at the repository root, shared with the app (app/CMakeLists.txt): the
# two read the same npcs.sqlite, so one copy between them is one version of the format.
add_library(sqlite3 STATIC ../third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ../third_party/sqlite3)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=0
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
)
if(NOT MSVC)
    target_compile_options(sqlite3 PRIVATE -w)
endif()

add_library(miniz STATIC vendor/miniz/miniz.c)
target_include_directories(miniz PUBLIC vendor/miniz)
if(NOT MSVC)
    target_compile_options(miniz PRIVATE -w)
endif()

add_executable(kcd2modgen
    src/main.cpp
    src/npcdb.cpp
    src/emit.cpp
    src/pak.cpp
)
target_link_libraries(kcd2modgen PRIVATE sqlite3 miniz)

# The other half of a conversation -- the part that writes the files the Lua cannot --
# is the app (app/), because it needs the model anyway and because two implementations
# of the same turn protocol would drift. `moonshine_app --game --canned` is the
# placeholder-turn version of it. scripts/build_mod.sh is what runs this generator.
