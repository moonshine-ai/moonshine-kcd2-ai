@echo off
rem scripts\fetch_moonshine.ps1 from a developer prompt: the build of moonshine.dll wants
rem MSVC and CMake on the PATH, which vcvars64 provides. This finds Visual Studio the way
rem scripts\build_windows.bat does, calls vcvars64, and runs the PowerShell script with
rem whatever arguments it was given (-NoBuild). Environment variables pass through
rem (MOONSHINE_ROOT is not read; MOONSHINE_REPO, MOONSHINE_PIN, MOONSHINE_LFS_FROM are).
setlocal
set "ROOT=%~dp0.."
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo no Visual Studio found by vswhere
    exit /b 1
)
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo no vcvars64.bat under "%VSDIR%"
    exit /b 1
)
call "%VCVARS%" >nul 2>&1 || (echo vcvars failed & exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\fetch_moonshine.ps1" %*
exit /b %ERRORLEVEL%
