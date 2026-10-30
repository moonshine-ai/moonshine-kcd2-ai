cmake_minimum_required(VERSION 3.20)
# C as well as C++, for the vendored SQLite amalgamation.
project(moonshine_app CXX C)

# 20 for designated initializers, which the Approach tests lean on heavily: an Approach has
# eight fields and a test that says {.blood = 0.5} is legible where one passing eight
# positional values is not. Clang takes them in 17 as an extension and MSVC does not, so the
# standard has to say so or the tests only build on one of the two machines.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cpp-httplib, nlohmann/json and doctest are vendored as single headers so a
# checkout builds with nothing installed, the same way the mod generator vendors
# sqlite and miniz. All three are MIT, which matters because this ships.
add_library(vendor INTERFACE)
target_include_directories(vendor INTERFACE vendor)

# SQLite is the one amalgamation at the root, shared with the mod generator (mod/):
# tools/assemble_db.py writes npcs.sqlite, the generator and the app both read it, so one
# copy between them is one version of the file format.
set(SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite3)
add_library(sqlite3 STATIC ${SQLITE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${SQLITE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
)
if(NOT MSVC)
    target_compile_options(sqlite3 PRIVATE -w)
endif()

# libopus, vendored so the phrase cache encodes and decodes with nothing installed:
# the app ships to a machine that has no package manager. Speech does not use
# the DNN, the tests or the programs, and they are most of the tree's build.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/opus EXCLUDE_FROM_ALL)

# llama.cpp, pinned as a submodule and built here rather than installed, because the point
# of moving off Ollama is that there is nothing to install: one folder to ship, and a KV
# cache that belongs to this process instead of to a server that may drop it.
#
# Optional so that a checkout without the submodule still builds and still passes its tests.
# Everything worth testing about a conversation is reachable with tests/stub_llm.h, and
# requiring 164 MB of submodule to run them would be a poor trade.
option(MOONSHINE_LLAMA "build the in-process llama.cpp backend" ON)
set(LLAMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp)
if(MOONSHINE_LLAMA AND NOT EXISTS ${LLAMA_DIR}/include/llama.h)
    message(WARNING "vendor/llama.cpp is empty; run git submodule update --init. "
                    "Building without the llama.cpp backend.")
    set(MOONSHINE_LLAMA OFF)
endif()

if(MOONSHINE_LLAMA)
    # Only the library. The tools, tests, examples and server are most of llama.cpp's build
    # time and none of them ship here -- the whole argument for going in-process is that
    # there is no server. LLAMA_CURL off because it would add a libcurl dependency for a
    # download path this app does not use.
    set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_UI       OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
    # libmtmd as well, standalone: it carries llama.cpp's Qwen3-TTS code predictor and
    # code2wav graphs, which the GPU clone engine (src/clone_llama.cpp) drives. Video needs
    # an ffmpeg subprocess that this app does not have.
    set(LLAMA_BUILD_MTMD     ON  CACHE BOOL "" FORCE)
    set(MTMD_VIDEO           OFF CACHE BOOL "" FORCE)
    # Static, so what ships is the app and the model and nothing else to find at load time.
    set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
    # A GPU backend for the voices. Metal comes with llama.cpp on macOS; CUDA is built when
    # the machine has the toolkit, for the architectures that cover the GeForce
    # cards this could ship to -- 86 and 89 as native code, 75 as PTX that older and newer
    # cards JIT -- rather than the one on the build machine (GGML_NATIVE is off above) or
    # llama.cpp's full list, which takes the better part of an hour to compile. The LLM and
    # the embedder stay on the CPU regardless (cpu_only, op_offload off): the backend is
    # there for src/clone_llama.cpp.
    option(MOONSHINE_CUDA "build ggml's CUDA backend when a CUDA toolkit is found" ON)
    set(MOONSHINE_CUDA_ARCHITECTURES "75-virtual;86-real;89-real" CACHE STRING
        "CMAKE_CUDA_ARCHITECTURES for the CUDA backend")
    if(MOONSHINE_CUDA AND NOT APPLE)
        include(CheckLanguage)
        check_language(CUDA)
        if(CMAKE_CUDA_COMPILER)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
            if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
                set(CMAKE_CUDA_ARCHITECTURES ${MOONSHINE_CUDA_ARCHITECTURES})
            endif()
            message(STATUS "CUDA backend on, for architectures ${CMAKE_CUDA_ARCHITECTURES}")
        else()
            message(STATUS "no CUDA toolkit found; the clones run on the CPU on this build")
        endif()
    endif()
    # GGML_NATIVE would build for whatever machine compiled it, which is wrong for something
    # that gets sent to another machine -- and the build host is not the player's.
    set(GGML_NATIVE          OFF CACHE BOOL "" FORCE)

    # Built as the C++17 project it says it is. src/CMakeLists.txt asks for cxx_std_17 and
    # writes "don't bump" beside it, and CMAKE_CXX_STANDARD from this file was bumping it
    # anyway, because a subdirectory inherits it and CMake takes whichever standard is higher.
    #
    # Not cosmetic. In C++20 a u8"..." literal is const char8_t*, which will not convert to
    # const char*, and llama-chat.cpp has dozens of them -- 15 hard errors on MSVC. Clang let
    # the same build through on macOS, so this only appeared on the machine that ships.
    # Local changes to llama.cpp, kept as patches in vendor/patches/llama.cpp and applied to
    # the checkout here, at configure time, so that the submodule stays a plain upstream pin
    # (every machine clones it from GitHub). Each patch is applied once: one
    # that already applies in reverse is in place and is left alone. A patch that fits
    # neither way means the pin moved under it, and that stops the configure rather than
    # building an app that quietly lacks the change.
    find_package(Git QUIET)
    file(GLOB LLAMA_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/vendor/patches/llama.cpp/*.patch)
    list(SORT LLAMA_PATCHES)
    foreach(patch IN LISTS LLAMA_PATCHES)
        if(NOT Git_FOUND)
            message(FATAL_ERROR "git is needed to apply ${patch} to vendor/llama.cpp")
        endif()
        execute_process(COMMAND ${GIT_EXECUTABLE} apply --check --reverse ${patch}
                        WORKING_DIRECTORY ${LLAMA_DIR} RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
        if(NOT already EQUAL 0)
            execute_process(COMMAND ${GIT_EXECUTABLE} apply ${patch}
                            WORKING_DIRECTORY ${LLAMA_DIR} RESULT_VARIABLE applied)
            if(NOT applied EQUAL 0)
                message(FATAL_ERROR "${patch} does not apply to vendor/llama.cpp at this pin")
            endif()
            get_filename_component(patch_name ${patch} NAME)
            message(STATUS "applied ${patch_name} to vendor/llama.cpp")
        endif()
    endforeach()

    set(MOONSHINE_CXX_STANDARD ${CMAKE_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD 17)
    add_subdirectory(${LLAMA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/llama.cpp EXCLUDE_FROM_ALL)
    set(CMAKE_CXX_STANDARD ${MOONSHINE_CXX_STANDARD})
endif()

add_library(moonshine_core STATIC
    src/prompt.cpp
    src/sentence.cpp
    src/prefill.cpp
    src/ollama.cpp
    src/serve.cpp
    src/memory.cpp
    src/approach.cpp
    src/gamelog.cpp
    src/turnfile.cpp
    src/session.cpp
    src/personadb.cpp
    src/inspect.cpp
    src/quest.cpp
    src/menu.cpp
    src/scripted.cpp
    src/knowledge.cpp
    src/console.cpp
    src/lipsync.cpp
    src/word_align.cpp
    src/visemes.cpp
    src/phrase_cache.cpp
)
target_include_directories(moonshine_core PUBLIC src)
target_link_libraries(moonshine_core PUBLIC vendor sqlite3 opus)

# The backend is compiled in only when llama.cpp is there, and the define is what the
# callers key off. Two ways to fail to have a backend -- a missing submodule and a
# deliberate -DMOONSHINE_LLAMA=OFF -- and one symbol to test for.
if(MOONSHINE_LLAMA)
    # The embedder for the knowledge base rides the same library: a second, small context.
    target_sources(moonshine_core PRIVATE src/llamacpp.cpp src/embedder.cpp)
    target_link_libraries(moonshine_core PUBLIC llama)
    target_compile_definitions(moonshine_core PUBLIC MOONSHINE_HAS_LLAMA=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(moonshine_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(moonshine_core PUBLIC ws2_32)
endif()

add_executable(moonshine_app src/main.cpp src/voice.cpp src/listen.cpp)
target_link_libraries(moonshine_app PRIVATE moonshine_core)

# The souls' voices are spoken in-process by Phonon, Gradium's 31M text-to-speech model,
# through the C interface of its Rust runtime, pinned as the submodule vendor/xn-ptts and
# built here by cargo (rustup.rs puts it on the PATH; nothing else is needed). The runtime
# is one shared library the exe needs beside it, as OpenBLAS was for the engine below; the
# model is 68 MB under models/phonon-31m-en (scripts/models.tsv), with the voices in it.
# Optional in the same way as llama.cpp -- without it the app still talks, it just does not
# speak -- and only the app links it; the tests compile voice.cpp without any engine.
#
# Off on macOS: that build of the runtime drives Core ML only and loads an exported bundle,
# not the checkpoint folder this ships. The app speaks there through Kokoro when built with
# Moonshine Voice, or not at all.
option(MOONSHINE_PHONON "speak in the Phonon voices through the vendored xn-ptts runtime" ON)
set(XN_PTTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/xn-ptts)
if(MOONSHINE_PHONON AND APPLE)
    message(STATUS "Phonon is off on macOS (its runtime there is Core ML only)")
    set(MOONSHINE_PHONON OFF)
endif()
if(MOONSHINE_PHONON AND NOT EXISTS ${XN_PTTS_DIR}/ptts-ffi/Cargo.toml)
    message(WARNING "vendor/xn-ptts is empty; run git submodule update --init. "
                    "Building without the Phonon voice.")
    set(MOONSHINE_PHONON OFF)
endif()
if(MOONSHINE_PHONON)
    find_program(CARGO cargo HINTS $ENV{USERPROFILE}/.cargo/bin $ENV{HOME}/.cargo/bin)
    if(NOT CARGO)
        message(WARNING "no cargo on the PATH (install Rust from https://rustup.rs). "
                        "Building without the Phonon voice.")
        set(MOONSHINE_PHONON OFF)
    endif()
endif()
if(MOONSHINE_PHONON)
    # The submodule's own .cargo/config.toml asks for the CPU of the build machine; this
    # ships to other machines, so the baseline is set here instead, the one the runtime's
    # own releases use. RUSTFLAGS in the environment replaces that file's setting.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        set(MOONSHINE_PHONON_TARGET_CPU "x86-64-v3" CACHE STRING "rustc target-cpu for the Phonon runtime")
    else()
        set(MOONSHINE_PHONON_TARGET_CPU "generic" CACHE STRING "rustc target-cpu for the Phonon runtime")
    endif()
    set(PTTS_TARGET_DIR ${CMAKE_BINARY_DIR}/xn-ptts)
    if(WIN32)
        set(PTTS_SHARED ${PTTS_TARGET_DIR}/release/ptts_ffi.dll)
        set(PTTS_IMPLIB ${PTTS_TARGET_DIR}/release/ptts_ffi.dll.lib)
    else()
        set(PTTS_SHARED ${PTTS_TARGET_DIR}/release/libptts_ffi.so)
        set(PTTS_IMPLIB ${PTTS_SHARED})
    endif()
    # cargo keeps its own account of what is up to date, so it runs on every build and is
    # a no-op in under a second when nothing changed. Always release: the debug build of
    # the runtime is too slow to speak with.
    add_custom_target(ptts_ffi_build
        COMMAND ${CMAKE_COMMAND} -E env
                "RUSTFLAGS=-C target-cpu=${MOONSHINE_PHONON_TARGET_CPU}"
                "CARGO_TARGET_DIR=${PTTS_TARGET_DIR}"
                ${CARGO} build --release --quiet -p ptts-ffi
        WORKING_DIRECTORY ${XN_PTTS_DIR}
        BYPRODUCTS ${PTTS_SHARED} ${PTTS_IMPLIB}
        COMMENT "cargo: the Phonon runtime (ptts-ffi, release, target-cpu=${MOONSHINE_PHONON_TARGET_CPU})"
        VERBATIM)
    add_library(ptts_ffi SHARED IMPORTED)
    set_target_properties(ptts_ffi PROPERTIES
        IMPORTED_LOCATION ${PTTS_SHARED}
        IMPORTED_IMPLIB ${PTTS_IMPLIB}
        INTERFACE_INCLUDE_DIRECTORIES ${XN_PTTS_DIR}/ptts-ffi/include)
    add_dependencies(moonshine_app ptts_ffi_build)
    target_sources(moonshine_app PRIVATE src/phonon.cpp)
    target_link_libraries(moonshine_app PRIVATE ptts_ffi)
    target_compile_definitions(moonshine_app PRIVATE MOONSHINE_HAS_PHONON=1)
    if(WIN32)
        target_link_libraries(moonshine_app PRIVATE winmm)
        add_custom_command(TARGET moonshine_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${PTTS_SHARED} $<TARGET_FILE_DIR:moonshine_app>
            VERBATIM)
    endif()
endif()

# The voices as they were spoken before Phonon (28 Oct 2026): cloned recordings through the
# Qwen3-TTS C engine, vendored under vendor/qwen3-tts and built as a static library, the
# model loaded once at startup and each line a generate call. Kept buildable and off: its
# model is 2.5 GB where Phonon's is 68, it holds 3.6 GB resident where Phonon holds 300 MB,
# and it speaks at real time where Phonon speaks at a tenth of it. Only the app links it;
# the tests compile voice.cpp without the engine. Kokoro stays the fallback where that
# library was found. Turned on beside Phonon, MOONSHINE_TTS_BACKEND=cpu picks it at runtime.
option(MOONSHINE_CLONE "speak in the cloned voices with the in-process Qwen3-TTS engine" OFF)
if(MOONSHINE_CLONE AND (APPLE OR WIN32))
    add_subdirectory(vendor/qwen3-tts EXCLUDE_FROM_ALL)
    target_sources(moonshine_app PRIVATE src/clone.cpp)
    target_link_libraries(moonshine_app PRIVATE qwen_tts)
    target_compile_definitions(moonshine_app PRIVATE MOONSHINE_HAS_CLONE=1)
    if(APPLE)
        target_link_libraries(moonshine_app PRIVATE "-framework AudioToolbox")
    elseif(WIN32)
        target_link_libraries(moonshine_app PRIVATE winmm)
        # OpenBLAS is the one DLL the exe needs beside it.
        add_custom_command(TARGET moonshine_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${QWEN_OPENBLAS_DLL} $<TARGET_FILE_DIR:moonshine_app>
            VERBATIM)
    endif()
endif()

# The same clones on the GPU, through llama.cpp. The vendored engine above is CPU-only and
# shares those cores with the game and the LLM; llama.cpp's Qwen3-TTS graphs run on whatever
# ggml backend this build has (Metal on macOS, CUDA where built) and leave the CPU alone. The talker
# is a libllama model, the code predictor and code2wav come from libmtmd, and the in-context
# voice prompt is built from the same .qvoice files. Picked at runtime when a GGUF pair is
# found beside the safetensors and a GPU device is present; the vendored engine otherwise.
# Off since Phonon, with the engine above.
option(MOONSHINE_GPU_CLONE "speak the cloned voices on the GPU through llama.cpp's Qwen3-TTS" OFF)
if(MOONSHINE_GPU_CLONE AND MOONSHINE_LLAMA AND TARGET mtmd)
    target_sources(moonshine_app PRIVATE src/clone_llama.cpp)
    target_link_libraries(moonshine_app PRIVATE mtmd)
    target_compile_definitions(moonshine_app PRIVATE MOONSHINE_HAS_GPU_CLONE=1)
    if(WIN32 AND GGML_CUDA)
        # The CUDA runtime and cuBLAS are the DLLs the exe then needs beside it, as OpenBLAS
        # is above: a machine without the toolkit has no other way to find them.
        find_package(CUDAToolkit REQUIRED)
        set(v ${CUDAToolkit_VERSION_MAJOR})
        foreach(dll cudart64_${v}.dll cublas64_${v}.dll cublasLt64_${v}.dll)
            add_custom_command(TARGET moonshine_app POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${CUDAToolkit_BIN_DIR}/${dll} $<TARGET_FILE_DIR:moonshine_app>
                VERBATIM)
        endforeach()
    endif()
endif()

# Moonshine Voice, through its C++ binding: Kokoro for the replies (MOONSHINE_TTS) and the
# transcriber for the player's own line while the talk button is held (MOONSHINE_STT; src/listen.cpp).
# Optional in the same way as llama.cpp: a checkout without the library still builds and
# still talks, it just does not speak, or hear. The binding is a header in the moonshine
# tree; the library is whatever that tree last built. scripts/fetch_moonshine.sh (.ps1 on
# Windows) clones the tree at its pin into third_party/moonshine and builds the library,
# which is where this looks by default; MOONSHINE_ROOT names another checkout. ONNX Runtime
# is loaded beside it by rpath, because the library asks for it as @rpath/libonnxruntime
# and does not carry a copy. On Windows the tree builds a DLL with an import library
# (-DMOONSHINE_BUILD_SHARED=YES, into core/build/Release/), and there is no rpath, so the
# two DLLs are copied beside the exe as OpenBLAS is above. The two options are separate
# because a machine can want one without the other: the Windows release speaks the clones
# through the Qwen3-TTS engine and never needed Kokoro, but hears through this library.
option(MOONSHINE_TTS "speak replies with Kokoro through Moonshine Voice" ON)
option(MOONSHINE_STT "hear the player through Moonshine Voice while the talk button is held" ON)
set(MOONSHINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/moonshine" CACHE PATH
    "moonshine checkout, for the Voice library and headers (scripts/fetch_moonshine.*)")
if(MOONSHINE_TTS OR MOONSHINE_STT)
    set(_moonshine_lib "")
    set(_moonshine_dll "")
    foreach(_name libmoonshine.dylib libmoonshine.so)
        if(EXISTS "${MOONSHINE_ROOT}/core/build/${_name}")
            set(_moonshine_lib "${MOONSHINE_ROOT}/core/build/${_name}")
            break()
        endif()
    endforeach()
    if(WIN32 AND NOT _moonshine_lib)
        foreach(_dir "${MOONSHINE_ROOT}/core/build/Release" "${MOONSHINE_ROOT}/core/build")
            if(EXISTS "${_dir}/moonshine.dll" AND EXISTS "${_dir}/moonshine.lib")
                set(_moonshine_lib "${_dir}/moonshine.lib")
                set(_moonshine_dll "${_dir}/moonshine.dll")
                break()
            endif()
        endforeach()
    endif()
    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_moonshine_ort "${MOONSHINE_ROOT}/core/third-party/onnxruntime/lib/macos/arm64")
        else()
            set(_moonshine_ort "${MOONSHINE_ROOT}/core/third-party/onnxruntime/lib/macos/x86_64")
        endif()
    elseif(WIN32)
        set(_moonshine_ort "${MOONSHINE_ROOT}/core/third-party/onnxruntime/lib/windows/x86_64")
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_moonshine_ort "${MOONSHINE_ROOT}/core/third-party/onnxruntime/lib/linux/aarch64")
        else()
            set(_moonshine_ort "${MOONSHINE_ROOT}/core/third-party/onnxruntime/lib/linux/x86_64")
        endif()
    endif()
    if(_moonshine_lib AND EXISTS "${MOONSHINE_ROOT}/core/moonshine-cpp.h" AND
       EXISTS "${_moonshine_ort}")
        if(MOONSHINE_TTS)
            target_compile_definitions(moonshine_app PRIVATE MOONSHINE_HAS_TTS=1)
        endif()
        if(MOONSHINE_STT)
            target_compile_definitions(moonshine_app PRIVATE MOONSHINE_HAS_STT=1)
        endif()
        target_include_directories(moonshine_app PRIVATE "${MOONSHINE_ROOT}/core")
        target_link_libraries(moonshine_app PRIVATE "${_moonshine_lib}")
        set_target_properties(moonshine_app PROPERTIES
            BUILD_RPATH "${_moonshine_ort};${MOONSHINE_ROOT}/core/build"
            INSTALL_RPATH "${_moonshine_ort};${MOONSHINE_ROOT}/core/build")
        if(APPLE)
            target_link_libraries(moonshine_app PRIVATE "-framework AudioToolbox")
        elseif(WIN32)
            target_link_libraries(moonshine_app PRIVATE winmm)
            foreach(_dll "${_moonshine_dll}" "${_moonshine_ort}/onnxruntime.dll")
                add_custom_command(TARGET moonshine_app POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_dll}" $<TARGET_FILE_DIR:moonshine_app>
                    VERBATIM)
            endforeach()
        endif()
    else()
        message(WARNING "Moonshine Voice was not found under ${MOONSHINE_ROOT}; "
                        "building without Kokoro or the ear. Run scripts/fetch_moonshine.sh "
                        "(.ps1 on Windows), set MOONSHINE_ROOT, or "
                        "-DMOONSHINE_TTS=OFF -DMOONSHINE_STT=OFF.")
    endif()
endif()

# The logic worth testing is all reachable without a model and without the game:
# what gets sent, and what is done with what comes back. tests/stub_llm.h stands
# in for the model, tests/fake_game.h for the game.
enable_testing()
add_executable(moonshine_tests
    tests/test_main.cpp
    tests/test_prompt.cpp
    tests/test_sentence.cpp
    tests/test_prefill.cpp
    tests/test_llamacpp.cpp
    tests/test_serve.cpp
    tests/test_memory.cpp
    tests/test_approach.cpp
    tests/test_gamelog.cpp
    tests/test_turnfile.cpp
    tests/test_session.cpp
    tests/test_personadb.cpp
    tests/test_inspect.cpp
    tests/test_quest.cpp
    tests/test_menu.cpp
    tests/test_scripted.cpp
    tests/test_knowledge.cpp
    tests/test_console.cpp
    tests/test_voice.cpp
    tests/test_phrase_cache.cpp
    tests/test_lipsync.cpp
    tests/test_word_align.cpp
    tests/test_visemes.cpp
    tests/test_listen.cpp
    tests/test_schema.cpp
    src/voice.cpp
    src/listen.cpp
)
target_include_directories(moonshine_tests PRIVATE tests vendor/doctest)
target_link_libraries(moonshine_tests PRIVATE moonshine_core)
# The shipped database's schema, which tests/test_schema.cpp runs and reads back through the
# app's own loaders: the one file tools/assemble_db.py builds npcs.sqlite from, so a column
# moved there and not here (or here and not there) fails a unit test.
set(MOONSHINE_SCHEMA_SQL "${CMAKE_CURRENT_SOURCE_DIR}/../data/schema.sql" CACHE FILEPATH
    "data/schema.sql, the shipped database's schema")
target_compile_definitions(moonshine_tests PRIVATE MOONSHINE_SCHEMA_SQL="${MOONSHINE_SCHEMA_SQL}")
add_test(NAME unit COMMAND moonshine_tests)
