# Moonshine Voice, the prebuilt library: download the Windows release archive into
# third_party\moonshine-voice (include\ and lib\) and check it against its pinned sha256.
# Nothing is compiled, so no Visual Studio prompt is needed. The app links it
# (app\CMakeLists.txt, MOONSHINE_ROOT) for the ear and for the voices: Phonon is in the
# library. The Windows half of scripts/fetch_moonshine.sh; the pin is the same.
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_moonshine.ps1 [-Force]
#
# What the archive holds: lib\moonshine.lib (static; everything but ONNX Runtime) with
# bin-tokenizer.lib, moonshine-utils.lib and ort-utils.lib beside it, which it needs at link
# time, and onnxruntime.lib with onnxruntime.dll, the one DLL the exe needs beside it
# (app\CMakeLists.txt copies it there). Wants tar on the PATH, which Windows 10 and later have.
param([switch]$Force)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$version = if ($env:MOONSHINE_VERSION) { $env:MOONSHINE_VERSION } else { 'v0.1.6' }
$base = if ($env:MOONSHINE_RELEASE_URL) { $env:MOONSHINE_RELEASE_URL } else { "https://github.com/moonshine-ai/moonshine/releases/download/$version" }
$archive = 'moonshine-voice-windows-x86_64.tar.gz'
# v0.1.6's. A pin that is moved without its hash is not the pin it was.
$want = '9e33fa88c1dd627314072ffe005af94888515c534cc599617462bc72ed3770f3'
if ($env:MOONSHINE_SHA256) {
    $want = $env:MOONSHINE_SHA256
} elseif ($version -ne 'v0.1.6') {
    throw "MOONSHINE_VERSION=$version needs MOONSHINE_SHA256 for its $archive."
}

$root = 'third_party\moonshine-voice'
$stamp = Join-Path $root '.release'
if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq "$version windows-x86_64") -and (Test-Path (Join-Path $root 'include\moonshine-cpp.h'))) {
    Write-Output "Moonshine Voice $version (windows-x86_64) is already in $root"
    exit 0
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("moonshine-voice-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    Write-Output "=== fetching $archive ($version) ==="
    $file = Join-Path $tmp $archive
    # The progress bar makes Invoke-WebRequest several times slower on a large file.
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri "$base/$archive" -OutFile $file -UseBasicParsing
    $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLower()
    if ($have -ne $want) { throw "$archive`: sha256 $have, want $want" }

    # Replace whatever was there, so a library from an earlier pin cannot be linked beside
    # this one's headers.
    if (Test-Path $root) { Remove-Item -Recurse -Force $root }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    & tar -xzf $file -C $root --strip-components=1
    if ($LASTEXITCODE -ne 0) { throw 'tar failed to unpack the archive' }
    Set-Content -Path $stamp -Value "$version windows-x86_64"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Get-ChildItem (Join-Path $root 'lib') | ForEach-Object { Write-Output ("  " + $_.Name) }
Write-Output "app\CMakeLists.txt finds it at MOONSHINE_ROOT=$((Get-Location).Path)\$root"
