# The Qwen3-TTS C engine as a static library for the companion app.
#
# Upstream builds with a GCC Makefile and has no CMake; this mirrors its flags. The engine
# needs a BLAS for prefill and the speech decoder (without one the SGEMM is a stub):
# Accelerate on macOS, OpenBLAS elsewhere. On Windows the official OpenBLAS release is
# fetched at configure time and its DLL is handed to the app to ship beside the exe.
#
# The library exposes one header to the app, qwen_tts_voice_file.h; the engine's own
# headers are not valid C++ and stay private.

set(QWEN_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(QWEN_ENGINE_SRCS
    qwen_tts_kleidi.c qwen_tts_q8repack.c qwen_tts_q4export.c
    qwen_tts.c qwen_tts_gguf.c qwen_tts_talker.c qwen_tts_code_predictor.c
    qwen_tts_speech_decoder.c qwen_tts_kernels.c qwen_tts_dispatch.c qwen_tts_costmap.c
    qwen_tts_sd_gemm.c qwen_tts_thread.c qwen_tts_kernels_generic.c qwen_tts_kernels_neon.c
    qwen_tts_kernels_avx.c qwen_tts_audio.c qwen_tts_emotion.c qwen_tts_compose.c
    qwen_tts_sampling.c qwen_tts_tokenizer.c qwen_json.c qwen_tts_voice_clone.c
    qwen_tts_speech_encoder.c qwen_tts_voice_file.c
    vendor/lz4.c)

# Ingot is used as a weights reader only (safetensors/gguf), so its SIMD kernels stay out:
# the "core" set plus dequant.c under INGOT_NO_KERNELS, as its own Makefile does.
set(QWEN_INGOT_SRCS
    third_party/ingot/src/dtype.c third_party/ingot/src/gguf.c third_party/ingot/src/safetensors.c
    third_party/ingot/src/wfile.c third_party/ingot/src/write.c third_party/ingot/src/dequant.c)

set(QWEN_ALL_SRCS ${QWEN_ENGINE_SRCS} ${QWEN_INGOT_SRCS})
if(MSVC)
    list(APPEND QWEN_ALL_SRCS compat/msvc/qwen_msvc_compat.c)
endif()

add_library(qwen_tts STATIC ${QWEN_ALL_SRCS})
set_target_properties(qwen_tts PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_include_directories(qwen_tts
    PUBLIC ${QWEN_DIR}
    PRIVATE ${QWEN_DIR}/vendor ${QWEN_DIR}/third_party/ingot/include)
target_compile_definitions(qwen_tts PRIVATE USE_BLAS INGOT_NO_KERNELS QWEN_NO_KLEIDI)

if(APPLE)
    target_compile_definitions(qwen_tts PRIVATE ACCELERATE_NEW_LAPACK QWEN_SIMD_PROFILE="native")
    target_compile_options(qwen_tts PRIVATE -O3 -ffast-math -march=native -Wno-unused-function
        -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable)
    target_link_libraries(qwen_tts PUBLIC "-framework Accelerate")
elseif(MSVC)
    # OpenBLAS: the official x64 release, pinned by hash.
    set(QWEN_OPENBLAS_VERSION 0.3.28)
    set(QWEN_OPENBLAS_URL
        "https://github.com/OpenMathLib/OpenBLAS/releases/download/v${QWEN_OPENBLAS_VERSION}/OpenBLAS-${QWEN_OPENBLAS_VERSION}-x64.zip")
    set(QWEN_OPENBLAS_SHA256 4cbd0e5daa3fb083b18f5e5fa6eefe79e2f2c51a6d539f98a3c6309a21160042)
    set(QWEN_OPENBLAS_ROOT ${CMAKE_BINARY_DIR}/openblas-${QWEN_OPENBLAS_VERSION})
    if(NOT EXISTS ${QWEN_OPENBLAS_ROOT}/bin/libopenblas.dll)
        set(_zip ${CMAKE_BINARY_DIR}/openblas-${QWEN_OPENBLAS_VERSION}.zip)
        if(NOT EXISTS ${_zip})
            message(STATUS "qwen_tts: fetching OpenBLAS ${QWEN_OPENBLAS_VERSION} for Windows")
            file(DOWNLOAD ${QWEN_OPENBLAS_URL} ${_zip} EXPECTED_HASH SHA256=${QWEN_OPENBLAS_SHA256}
                 SHOW_PROGRESS STATUS _dl)
            list(GET _dl 0 _dl_code)
            if(NOT _dl_code EQUAL 0)
                file(REMOVE ${_zip})
                message(FATAL_ERROR "qwen_tts: OpenBLAS download failed: ${_dl}")
            endif()
        endif()
        file(ARCHIVE_EXTRACT INPUT ${_zip} DESTINATION ${QWEN_OPENBLAS_ROOT})
    endif()
    add_library(qwen_openblas SHARED IMPORTED GLOBAL)
    set_target_properties(qwen_openblas PROPERTIES
        IMPORTED_LOCATION ${QWEN_OPENBLAS_ROOT}/bin/libopenblas.dll
        IMPORTED_IMPLIB ${QWEN_OPENBLAS_ROOT}/libopenblas.lib
        INTERFACE_INCLUDE_DIRECTORIES ${QWEN_OPENBLAS_ROOT}/include)
    # The app copies this beside its executable.
    set(QWEN_OPENBLAS_DLL ${QWEN_OPENBLAS_ROOT}/bin/libopenblas.dll CACHE INTERNAL "OpenBLAS runtime")

    # QWEN_USE_PTHREADS: the engine's own Win32 pool backend deadlocks when the talker
    # loop and the overlapped speech decoder submit work at the same time, which is the
    # normal shape of a generate call. The pthread backend (the one every Linux run uses)
    # runs here over the pthread shim in compat/msvc and has the decoder lane the Win32
    # one lacks.
    target_compile_definitions(qwen_tts PRIVATE USE_OPENBLAS QWEN_SIMD_PROFILE="portable"
        QWEN_USE_PTHREADS _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(qwen_tts PRIVATE ${QWEN_DIR}/compat/msvc)
    # C17 with C11 atomics (still behind the experimental switch), AVX2+FMA like upstream's
    # "portable" profile, fast math like upstream's -ffast-math.
    # An implicitly declared function (/we4013) is an error: on x64 it would truncate a
    # returned pointer to int and crash later, which is worse than not linking.
    target_compile_options(qwen_tts PRIVATE /experimental:c11atomics /arch:AVX2 /fp:fast /O2 /Ob3 /Ot /GS-
        /Zc:preprocessor /we4013 /wd4244 /wd4267 /wd4305 /wd4996 /wd4018 /wd4101 /wd4146 /wd4028 /wd4047 /wd4024)
    # Whole-program optimisation across the kernel files; the app links with /LTCG.
    set_target_properties(qwen_tts PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
                                              INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    # Every engine file sees the POSIX/GCC shims; the shim's own implementation does not.
    foreach(_src IN LISTS QWEN_ENGINE_SRCS QWEN_INGOT_SRCS)
        set_property(SOURCE ${_src} APPEND PROPERTY COMPILE_OPTIONS
            "/FI${QWEN_DIR}/compat/msvc/qwen_msvc_compat.h")
    endforeach()
    target_link_libraries(qwen_tts PUBLIC qwen_openblas)
else()
    target_compile_definitions(qwen_tts PRIVATE USE_OPENBLAS QWEN_SIMD_PROFILE="portable")
    target_compile_options(qwen_tts PRIVATE -O3 -ffast-math -mavx2 -mfma -Wno-unused-function
        -Wno-unused-variable -Wno-unused-parameter)
    target_include_directories(qwen_tts PRIVATE /usr/include/openblas)
    target_link_libraries(qwen_tts PUBLIC openblas pthread m)
endif()
