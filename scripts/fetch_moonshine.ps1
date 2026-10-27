# Moonshine Voice, for the ear: clone the moonshine tree at its pin into third_party\moonshine
# and build moonshine.dll, which the app links (app\CMakeLists.txt, MOONSHINE_ROOT) and the
# installer ships beside the exe with onnxruntime.dll. The Windows half of
# scripts/fetch_moonshine.sh; the pin is the same.
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_moonshine.ps1 [-NoBuild]
#
# Wants git (with git-lfs), cmake and the Visual Studio build tools on the PATH; run it from
# a developer prompt, or let scripts\build_windows.bat run it. The tree keeps two generated
# source files in Git LFS (its Windows ONNX Runtime is plain git); when GitHub's LFS quota
# for the repository is spent, MOONSHINE_LFS_FROM names another moonshine checkout whose
# object store has them, as in the .sh.
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$repo = if ($env:MOONSHINE_REPO) { $env:MOONSHINE_REPO } else { 'https://github.com/moonshine-ai/moonshine' }
$pin = if ($env:MOONSHINE_PIN) { $env:MOONSHINE_PIN } else { '9b05faab8adf4367e3c81bf05cd49207d3788e8e' }
$root = 'third_party\moonshine'

$env:GIT_LFS_SKIP_SMUDGE = '1'
if (-not (Test-Path (Join-Path $root '.git'))) {
    Write-Output "=== cloning moonshine into $root ==="
    & git clone --filter=blob:none $repo $root
    if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
}
$have = (& git -C $root rev-parse HEAD).Trim()
if ($have -ne $pin) {
    Write-Output "=== checking out $pin ==="
    & git -C $root fetch --quiet origin $pin
    & git -C $root checkout --quiet $pin
    if ($LASTEXITCODE -ne 0) { throw "checkout of $pin failed" }
}
& git -C $root log --oneline -1

# The LFS objects a Windows build compiles: the two generated sources. A pointer file is a
# few lines; a real one is megabytes.
$needed = @(& git -C $root lfs ls-files -n) | Where-Object { $_ -match '^core/(cpp-annote|moonshine-tts/src)/' }
$missing = @($needed | Where-Object { $p = Join-Path $root $_; -not (Test-Path -LiteralPath $p) -or (Get-Item -LiteralPath $p).Length -lt 1024 })
if ($missing.Count -gt 0) {
    Write-Output "=== pulling $($missing.Count) LFS objects ==="
    if ($env:MOONSHINE_LFS_FROM) {
        # From that checkout's object store, or, when git lfs prune has emptied it, from its
        # working tree where the file is the same object (the sha256 is the oid).
        $from = Join-Path $env:MOONSHINE_LFS_FROM '.git\lfs\objects'
        foreach ($f in $missing) {
            $oid = ((& git -C $root lfs ls-files -l -I $f) -split '\s+')[0]
            $src = Join-Path $from (Join-Path $oid.Substring(0, 2) (Join-Path $oid.Substring(2, 2) $oid))
            if (-not (Test-Path -LiteralPath $src)) {
                $tree = Join-Path $env:MOONSHINE_LFS_FROM ($f -replace '/', '\')
                if ((Test-Path -LiteralPath $tree) -and (Get-FileHash -Algorithm SHA256 -LiteralPath $tree).Hash.ToLower() -eq $oid) {
                    $src = $tree
                } else {
                    throw "$($env:MOONSHINE_LFS_FROM) has no object for $f"
                }
            }
            $dst = Join-Path $root (Join-Path '.git\lfs\objects' (Join-Path $oid.Substring(0, 2) $oid.Substring(2, 2)))
            New-Item -ItemType Directory -Force -Path $dst | Out-Null
            Copy-Item -LiteralPath $src -Destination (Join-Path $dst $oid) -Force
        }
        & git -C $root lfs checkout @missing
        if ($LASTEXITCODE -ne 0) { throw 'git lfs checkout failed' }
    } else {
        & git -C $root lfs pull --include ($missing -join ',')
        if ($LASTEXITCODE -ne 0) {
            throw 'git lfs pull failed. If GitHub says the repository exceeded its LFS budget, set MOONSHINE_LFS_FROM to a moonshine checkout that has the objects and run again.'
        }
    }
}

if (-not $NoBuild) {
    Write-Output '=== building moonshine.dll ==='
    $core = Join-Path $root 'core'
    & cmake -S $core -B (Join-Path $core 'build') -DMOONSHINE_BUILD_SHARED=YES
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
    & cmake --build (Join-Path $core 'build') --config Release --target moonshine
    if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }
    Get-ChildItem (Join-Path $core 'build\Release') -Filter 'moonshine.*' | ForEach-Object { Write-Output ("  " + $_.Name) }
}
Write-Output "app\CMakeLists.txt finds it at MOONSHINE_ROOT=$((Get-Location).Path)\$root"
