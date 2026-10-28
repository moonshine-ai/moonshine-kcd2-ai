@echo off
rem Build and test the app on Windows, where it ships.
rem
rem A batch file rather than a command line: the quoting needed to call vcvars64 and then
rem cmake through powershell (or ssh) does not survive the trip, and this is the shape
rem that does.
rem
rem   scripts\build_windows.bat            the development build, app\build\RelWithDebInfo\
rem   scripts\build_windows.bat release    the build that ships to testers, app\build-release\RelWithDebInfo\
rem
rem The release build is the same sources with the CUDA backend left out. The app runs
rem the model, the embedder and the voice on the CPU by default and the GPU only as an
rem experiment (--gpu, MOONSHINE_TTS_BACKEND=gpu), the three CUDA DLLs are 600 MB a
rem tester would download for nothing, and the embedder's CUDA fault beside the game's
rem own GPU use is a known crash. Its own build directory, since CMake caches GGML_CUDA
rem once it is on and the app's CMakeLists only ever forces it on. What ships still wants
rem the VC++ runtime: moonshine.dll and onnxruntime.dll import MSVCP140 and VCRUNTIME140
rem whatever this exe is linked against, so the installer carries vc_redist.x64.exe
rem rather than this build carrying a static CRT.
rem
rem Wants: Visual Studio 2022 (Build Tools are enough) with the C++ workload and CMake, git
rem with the llama.cpp and xn-ptts submodules checked out (git submodule update --init), Rust
rem (cargo, from https://rustup.rs) for the voice's runtime, and, for the
rem ear, Moonshine Voice under third_party\moonshine (scripts\fetch_moonshine.ps1; run from
rem this file's developer prompt, or MOONSHINE_ROOT names another checkout).
setlocal

rem This file's parent's parent: the checkout's root.
set "ROOT=%~dp0.."
set "BUILD=app\build"
set "RELEASE_FLAGS="
rem The argument with any quote removed: Windows OpenSSH hands `... build_windows.bat release`
rem on with a stray quote on the end of it, and `release"` is not `release`.
set "MODE=%~1"
if defined MODE set "MODE=%MODE:"=%"
if /i "%MODE%"=="release" (
    set "BUILD=app\build-release"
    set "RELEASE_FLAGS=-DMOONSHINE_CUDA=OFF -DGGML_CUDA=OFF"
)
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

rem vswhere rather than a wildcard search. `dir /s` does not expand a wildcard in the
rem middle of a path, which is why an earlier version of this reported no toolchain on
rem a machine that has one.
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo no Visual Studio found by vswhere
    exit /b 1
)
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
rem Quoted inside the block: Build Tools live under "Program Files (x86)", and an unquoted
rem ")" in an expanded variable closes the block early ("\Microsoft was unexpected").
if not exist "%VCVARS%" (
    echo no vcvars64.bat under "%VSDIR%"
    exit /b 1
)

echo using %VCVARS%
call "%VCVARS%" >nul 2>&1 || (echo vcvars failed & exit /b 1)

cd /d "%ROOT%" || exit /b 1
if not exist app\vendor\llama.cpp\CMakeLists.txt (
    echo app\vendor\llama.cpp is empty: git submodule update --init
    exit /b 1
)
if not exist app\vendor\xn-ptts\ptts-ffi\Cargo.toml (
    echo app\vendor\xn-ptts is empty: git submodule update --init
    exit /b 1
)
rem cargo, for the voice's runtime. rustup puts it under %USERPROFILE%\.cargo\bin, which a
rem session begun before the install (an ssh one, say) does not have on its PATH yet.
where cargo >nul 2>&1 || set "PATH=%USERPROFILE%\.cargo\bin;%PATH%"
where cargo >nul 2>&1 || (
    echo no cargo on the PATH: install Rust from https://rustup.rs for the voice
    exit /b 1
)
rem Ninja, which vcvars64 has just put on the PATH, in its multi-config form so that the
rem binaries land in app\build\RelWithDebInfo\ as they did under the Visual Studio
rem generator. The difference is the CUDA backend: MSBuild compiles its 189 .cu files one
rem at a time, an hour and more, and ninja compiles them on every core. A build directory
rem left by the other generator is cleared first, since CMake will not reconfigure across
rem generators.
set "GEN=Ninja Multi-Config"
where ninja >nul 2>&1 || set "GEN=Visual Studio 17 2022"
if exist %BUILD%\CMakeCache.txt (
    findstr /c:"CMAKE_GENERATOR:INTERNAL=%GEN%" %BUILD%\CMakeCache.txt >nul 2>&1 || (
        echo clearing %BUILD%, made by another generator
        rmdir /s /q %BUILD%
    )
)
rem The Moonshine Voice library, for the ear at the menu's say row (app/src/listen.cpp):
rem third_party\moonshine, built as a DLL by scripts\fetch_moonshine.ps1, or MOONSHINE_ROOT.
rem Kokoro stays off: this build speaks through Phonon (app\vendor\xn-ptts, built by cargo
rem at configure time; rustup.rs puts cargo on the PATH), and the Kokoro build would ask for
rem its assets at start-up. Without the library the app builds
rem as before, without the ear, and the installer's staging step refuses the result.
if not defined MOONSHINE_ROOT set "MOONSHINE_ROOT=%ROOT%\third_party\moonshine"
set "MOONSHINE="
set "MROOT=%MOONSHINE_ROOT:\=/%"
if exist "%MOONSHINE_ROOT%\core\build\Release\moonshine.dll" (
    set "MOONSHINE=-DMOONSHINE_ROOT=%MROOT% -DMOONSHINE_TTS=OFF"
    echo ear: Moonshine Voice from %MROOT%
) else (
    echo no moonshine.dll under %MROOT%; building without the ear ^(scripts\fetch_moonshine.ps1^)
)
if "%GEN%"=="Ninja Multi-Config" (
    cmake -S app -B %BUILD% -G "%GEN%" %MOONSHINE% %RELEASE_FLAGS% || exit /b 1
) else (
    cmake -S app -B %BUILD% -G "%GEN%" -A x64 %MOONSHINE% %RELEASE_FLAGS% || exit /b 1
)
rem Half the cores: the game is often running on the other half.
set /a JOBS=%NUMBER_OF_PROCESSORS%/2
if %JOBS% lss 1 set JOBS=1
cmake --build %BUILD% --config RelWithDebInfo -j %JOBS% || exit /b 1

echo === tests ===
%BUILD%\RelWithDebInfo\moonshine_tests.exe
exit /b %ERRORLEVEL%
