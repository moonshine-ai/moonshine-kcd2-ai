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
rem the VC++ runtime: onnxruntime.dll imports MSVCP140 and VCRUNTIME140
rem whatever this exe is linked against, so the installer carries vc_redist.x64.exe
rem rather than this build carrying a static CRT.
rem
rem Wants: Visual Studio 2022 17.14 (MSVC toolset 14.44) or later, Build Tools being enough,
rem with the C++ workload and CMake. The prebuilt Moonshine library was built with a
rem toolset newer than 14.41 and calls STL helpers (__std_search_1 and friends) that 14.41
rem lacks, so a build there fails to link (10 Nov 2026, wl1). Also git
rem with the llama.cpp submodule checked out (git submodule update --init), and Moonshine
rem Voice's prebuilt release under third_party\moonshine-voice (scripts\fetch_moonshine.ps1;
rem MOONSHINE_ROOT names another unpacked release). The ear and the voice both come from
rem it: nothing of Moonshine is compiled here, and no Rust is needed.
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
rem A build directory whose cached compiler is gone (a Visual Studio update replaces the
rem toolset folder, 14.41 for 14.44) cannot be reconfigured either: cleared the same way.
if exist %BUILD%\CMakeCache.txt (
    for /f "tokens=2 delims==" %%c in ('findstr /b /c:"CMAKE_CXX_COMPILER:" %BUILD%\CMakeCache.txt') do (
        if not exist "%%c" (
            echo clearing %BUILD%, its compiler %%c is gone
            rmdir /s /q %BUILD%
        )
    )
)
rem The Moonshine Voice release, for the ear behind the talk button (app/src/listen.cpp)
rem and the voice (app/src/phonon.cpp, Moonshine's Phonon): third_party\moonshine-voice,
rem fetched by scripts\fetch_moonshine.ps1, or MOONSHINE_ROOT. It is linked statically, with
rem onnxruntime.dll beside it, which the build copies next to the exe. Kokoro stays off:
rem this build speaks through Phonon, and the Kokoro build would ask for its assets at
rem start-up. Without the library the app builds as before, without the ear and the
rem voice, and the installer's staging step refuses the result.
if not defined MOONSHINE_ROOT set "MOONSHINE_ROOT=%ROOT%\third_party\moonshine-voice"
set "MOONSHINE="
rem The default release folder is fetched when it is not there yet (a download of about
rem 40 MB, checked against its pinned sha256); a MOONSHINE_ROOT of your own is not touched.
if "%MOONSHINE_ROOT%"=="%ROOT%\third_party\moonshine-voice" if not exist "%MOONSHINE_ROOT%\lib\moonshine.lib" (
    echo fetching the Moonshine Voice release
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\fetch_moonshine.ps1 || exit /b 1
)
set "MROOT=%MOONSHINE_ROOT:\=/%"
if exist "%MOONSHINE_ROOT%\lib\moonshine.lib" (
    rem Quoted: a checkout under a home folder with a space in it (C:\Users\Pete Warden on
    rem wl2) otherwise reaches cmake as two arguments, and the ear is silently left out.
    set "MOONSHINE=-DMOONSHINE_ROOT="%MROOT%" -DMOONSHINE_TTS=OFF"
    echo ear and voice: Moonshine Voice from %MROOT%
) else (
    echo no moonshine.lib under %MROOT%\lib; building without the ear ^(scripts\fetch_moonshine.ps1^)
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

rem The tests take twenty-odd seconds (406 cases, many of which build a database), and a
rem deploy that changed nothing in the app does not need them again. MOONSHINE_TESTS says:
rem   always (the default)  run them
rem   changed               run them unless moonshine_tests.exe is no newer than the last
rem                         run that passed (tests.passed beside it), which is to say
rem                         unless something it is built from has changed
rem   skip                  do not run them
rem scripts/deploy_wl1.sh --fast sets "changed"; a release build leaves the default.
set "TESTS=%BUILD%\RelWithDebInfo\moonshine_tests.exe"
set "STAMP=%BUILD%\RelWithDebInfo\tests.passed"
if /i "%MOONSHINE_TESTS%"=="skip" (
    echo tests not run: MOONSHINE_TESTS=skip
    exit /b 0
)
if /i "%MOONSHINE_TESTS%"=="changed" if exist "%STAMP%" (
    powershell -NoProfile -Command "if ((Get-Item -LiteralPath '%TESTS%').LastWriteTimeUtc -le (Get-Item -LiteralPath '%STAMP%').LastWriteTimeUtc) { exit 0 } else { exit 1 }"
    if not errorlevel 1 (
        echo tests not run: moonshine_tests.exe is unchanged since the last run that passed
        exit /b 0
    )
)
echo === tests ===
%TESTS%
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" echo passed> "%STAMP%"
exit /b %RC%
