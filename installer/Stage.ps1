# Assemble the tester package on the Windows build machine, where the Windows build is,
# and then build the installer out of it with Inno Setup.
#
#   powershell -ExecutionPolicy Bypass -File installer\Stage.ps1 -Version 0.9.94 [-SkipInstaller]
#
# Everything it packs is in this checkout: the release build (scripts\build_windows.bat
# release), the assembled drop (scripts\fetch_drop.ps1), the models (scripts\fetch_models.ps1)
# and the mod folder (scripts\build_mod.sh, or a copy of its output under release\mod). The
# tree it builds, under <Repo>\release\stage\, is what moonshine.iss packs:
#
#   Moonshine\                      the installed folder, as it lands on a tester's disk
#     moonshine_app.exe + DLLs      app\build-release\RelWithDebInfo\ (CPU-only build)
#     Moonshine.ps1, Collect-Logs.ps1, README.txt, VERSION.txt, LICENSES\
#     setting.txt
#     data\kcd2\npcs.sqlite         a vacuumed copy of the assembled database
#     data\kcd2\voices\             clones\*.qvoice, phrases\, voices.txt, facials.txt, lexicon.tsv
#     models\embeddinggemma-300m-q8_0.gguf
#     models\qwen3-tts-0.6b\
#     models\llm\<name>.gguf        the one GGUF under models\llm
#     models\stt\tiny-streaming-en\ the one folder under models\stt
#   extra\vc_redist.x64.exe         run by the installer, not installed
#   mod\moonshine_names\            from release\mod, for the installer's optional copy task
#
# Every copy is a robocopy or a hash-checked Copy-Item, so a second run only moves what
# changed; the big weights are the same bytes from one build to the next. -Repo is this
# checkout unless told otherwise; -Python is the first python.exe on the PATH unless told.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$Python = $env:PYTHON,
    [switch]$SkipInstaller
)
$ErrorActionPreference = 'Stop'
if (-not $Python) {
    $cmd = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1
    if (-not $cmd) { Write-Error 'no python on the PATH; -Python C:\path\to\python.exe'; exit 1 }
    $Python = $cmd.Source
}
$stage = Join-Path $Repo 'release\stage'
$pkg = Join-Path $stage 'Moonshine'
$build = Join-Path $Repo 'app\build-release\RelWithDebInfo'
$installerSrc = Join-Path $Repo 'installer'

function Step([string]$text) { Write-Output ("=== " + $text) }
function Fail([string]$text) { Write-Error $text; exit 1 }
function Ensure-Dir([string]$path) { New-Item -ItemType Directory -Force -Path $path | Out-Null }

# Copy one file unless the destination already has the same size and hash.
function Copy-Same([string]$from, [string]$to) {
    if (-not (Test-Path -LiteralPath $from)) { Fail "missing: $from" }
    Ensure-Dir (Split-Path -Parent $to)
    if (Test-Path -LiteralPath $to) {
        $a = Get-Item -LiteralPath $from
        $b = Get-Item -LiteralPath $to
        if ($a.Length -eq $b.Length -and $a.LastWriteTimeUtc -eq $b.LastWriteTimeUtc) { return }
        if ($a.Length -eq $b.Length) {
            # Same size but a different time (a re-downloaded weight):
            # the hash decides. MD5 over a 2 GB file is seconds; this is not a security check.
            if ((Get-FileHash -LiteralPath $from -Algorithm MD5).Hash -eq (Get-FileHash -LiteralPath $to -Algorithm MD5).Hash) { return }
        }
    }
    Write-Output ("  " + (Split-Path -Leaf $to))
    Copy-Item -LiteralPath $from -Destination $to -Force
}

# Mirror a folder: robocopy with /MIR, which copies only what differs and removes what
# is gone from the source. Exit codes under 8 are success.
function Mirror([string]$from, [string]$to, [string[]]$excludeDirs = @(), [string[]]$excludeFiles = @()) {
    if (-not (Test-Path -LiteralPath $from)) { Fail "missing: $from" }
    Ensure-Dir $to
    $args = @($from, $to, '/MIR', '/NFL', '/NDL', '/NJH', '/NP', '/R:2', '/W:2')
    if ($excludeDirs.Count -gt 0) { $args += '/XD'; $args += $excludeDirs }
    if ($excludeFiles.Count -gt 0) { $args += '/XF'; $args += $excludeFiles }
    & robocopy @args | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "robocopy $from -> $to failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

Step "staging Moonshine $Version under $stage"
Ensure-Dir $pkg

# --- the program ---------------------------------------------------------------------------
Step 'program'
$exe = Join-Path $build 'moonshine_app.exe'
if (-not (Test-Path -LiteralPath $exe)) { Fail "no release build at $exe -- run scripts\build_windows.bat release" }
$dlls = @(Get-ChildItem -LiteralPath $build -Filter '*.dll')
$cuda = @($dlls | Where-Object { $_.Name -match '^(cublas|cudart|cublasLt|ggml-cuda)' })
if ($cuda.Count -gt 0) { Fail ("the release build has CUDA DLLs beside it: " + (($cuda | ForEach-Object Name) -join ', ') + " -- that is not the release build") }
foreach ($need in @('libopenblas.dll', 'moonshine.dll', 'onnxruntime.dll')) {
    if (-not ($dlls | Where-Object Name -eq $need)) { Fail "the release build is missing $need" }
}
Copy-Same $exe (Join-Path $pkg 'moonshine_app.exe')
foreach ($dll in $dlls) { Copy-Same $dll.FullName (Join-Path $pkg $dll.Name) }
# Anything else left in the package root from an earlier build.
Get-ChildItem -LiteralPath $pkg -File | Where-Object { $_.Extension -in '.dll', '.exe', '.pdb' -and -not (Test-Path -LiteralPath (Join-Path $build $_.Name)) } |
    ForEach-Object { Write-Output ("  removing " + $_.Name); Remove-Item -LiteralPath $_.FullName -Force }

foreach ($name in @('Moonshine.ps1', 'Collect-Logs.ps1')) {
    Copy-Same (Join-Path $installerSrc $name) (Join-Path $pkg $name)
}
Copy-Same (Join-Path $Repo 'TESTERS.md') (Join-Path $pkg 'README.txt')
Mirror (Join-Path $installerSrc 'LICENSES') (Join-Path $pkg 'LICENSES')
Set-Content -LiteralPath (Join-Path $pkg 'VERSION.txt') -Value $Version -NoNewline
Copy-Same (Join-Path $Repo 'setting.txt') (Join-Path $pkg 'setting.txt')

# --- the database --------------------------------------------------------------------------
# The assembled DB (scripts\fetch_drop.ps1) is already a clean single file; VACUUM INTO
# copies it from a consistent read all the same, so a DB the app has open, or one somebody
# edited by hand, ships whole.
Step 'database'
$db = Join-Path $Repo 'data\kcd2\npcs.sqlite'
if (-not (Test-Path -LiteralPath $db)) { Fail "no database at $db -- run scripts\fetch_drop.ps1" }
$dbOut = Join-Path $pkg 'data\kcd2\npcs.sqlite'
Ensure-Dir (Split-Path -Parent $dbOut)
$stampFile = "$dbOut.from"
$srcStamp = (Get-Item -LiteralPath $db).LastWriteTimeUtc.Ticks.ToString()
if ((Test-Path -LiteralPath $dbOut) -and (Test-Path -LiteralPath $stampFile) -and ((Get-Content -LiteralPath $stampFile) -eq $srcStamp)) {
    Write-Output '  unchanged'
} else {
    if (Test-Path -LiteralPath $dbOut) { Remove-Item -LiteralPath $dbOut -Force }
    $py = @"
import sqlite3, sys
src, dst = sys.argv[1], sys.argv[2]
con = sqlite3.connect(src)
con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
con.execute("VACUUM INTO ?", (dst,))
con.close()
chk = sqlite3.connect(dst)
print("  integrity:", chk.execute("PRAGMA integrity_check").fetchone()[0])
print("  tables:", chk.execute("select count(*) from sqlite_master where type='table'").fetchone()[0])
chk.close()
"@
    $pyFile = Join-Path $env:TEMP 'moonshine_vacuum.py'
    Set-Content -LiteralPath $pyFile -Value $py
    & $Python $pyFile $db $dbOut
    if ($LASTEXITCODE -ne 0) { Fail 'VACUUM INTO failed' }
    Remove-Item -LiteralPath $pyFile -Force
    Set-Content -LiteralPath $stampFile -Value $srcStamp -NoNewline
    Write-Output ("  " + [math]::Round((Get-Item -LiteralPath $dbOut).Length / 1MB) + " MB")
}

# --- the voices ----------------------------------------------------------------------------
Step 'voices'
$voices = Join-Path $Repo 'data\kcd2\voices'
$voicesOut = Join-Path $pkg 'data\kcd2\voices'
Mirror (Join-Path $voices 'clones') (Join-Path $voicesOut 'clones')
Mirror (Join-Path $voices 'phrases') (Join-Path $voicesOut 'phrases')
foreach ($name in @('facials.txt', 'lexicon.tsv')) {
    Copy-Same (Join-Path $voices $name) (Join-Path $voicesOut $name)
}
# voices.txt names the Kokoro voices, which this build does not carry; the app reads it
# only when it is there. Shipped when the checkout has it, so the catalogue is complete.
if (Test-Path -LiteralPath (Join-Path $voices 'voices.txt')) { Copy-Same (Join-Path $voices 'voices.txt') (Join-Path $voicesOut 'voices.txt') }
$clones = @(Get-ChildItem -LiteralPath (Join-Path $voicesOut 'clones') -Filter '*.qvoice')
if ($clones.Count -eq 0) { Fail 'no .qvoice files in the clones folder' }
Write-Output ("  " + $clones.Count + " cloned voices")

# --- the models ----------------------------------------------------------------------------
# All four come from scripts\fetch_models.ps1 (scripts\models.tsv pins them), which is the
# check that they are the right bytes; this only insists that they are there.
Step 'models'
$models = Join-Path $Repo 'models'
$modelsOut = Join-Path $pkg 'models'
Copy-Same (Join-Path $models 'embeddinggemma-300m-q8_0.gguf') (Join-Path $modelsOut 'embeddinggemma-300m-q8_0.gguf')
Mirror (Join-Path $models 'qwen3-tts-0.6b') (Join-Path $modelsOut 'qwen3-tts-0.6b') @('.cache')
if (-not (Test-Path -LiteralPath (Join-Path $modelsOut 'qwen3-tts-0.6b\model.safetensors'))) { Fail 'qwen3-tts-0.6b has no model.safetensors' }

# The language model: the one GGUF under models\llm, which the launcher passes as --gguf.
$ggufs = @(Get-ChildItem -LiteralPath (Join-Path $models 'llm') -Filter '*.gguf' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($ggufs.Count -ne 1) { Fail "models\llm should hold exactly one .gguf (has $($ggufs.Count)) -- scripts\fetch_models.ps1" }
$llmName = $ggufs[0].Name
$llmOut = Join-Path $modelsOut "llm\$llmName"
Ensure-Dir (Join-Path $modelsOut 'llm')
Get-ChildItem -LiteralPath (Join-Path $modelsOut 'llm') -Filter '*.gguf' | Where-Object Name -ne $llmName |
    ForEach-Object { Write-Output ("  removing " + $_.Name); Remove-Item -LiteralPath $_.FullName -Force }
Copy-Same $ggufs[0].FullName $llmOut
Write-Output ("  llm: $llmName, " + [math]::Round((Get-Item -LiteralPath $llmOut).Length / 1MB) + " MB")

# The ear: the one folder under models\stt, which the launcher passes as --stt-model.
$stts = @(Get-ChildItem -LiteralPath (Join-Path $models 'stt') -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
if ($stts.Count -ne 1) { Fail "models\stt should hold exactly one model folder (has $($stts.Count)) -- scripts\fetch_models.ps1" }
$Stt = $stts[0].Name
if (-not (Test-Path -LiteralPath (Join-Path $stts[0].FullName 'tokenizer.bin'))) { Fail "models\stt\$Stt has no tokenizer.bin" }
Mirror $stts[0].FullName (Join-Path $modelsOut "stt\$Stt")
Get-ChildItem -LiteralPath (Join-Path $modelsOut 'stt') -Directory | Where-Object Name -ne $Stt |
    ForEach-Object { Write-Output ("  removing " + $_.Name); Remove-Item -LiteralPath $_.FullName -Recurse -Force }

# --- what the installer runs or offers but does not install ---------------------------------
Step 'extras'
$extra = Join-Path $stage 'extra'
Ensure-Dir $extra
$redist = Join-Path $extra 'vc_redist.x64.exe'
if (-not (Test-Path -LiteralPath $redist) -or (Get-Item -LiteralPath $redist).Length -lt 10MB) {
    Write-Output '  downloading vc_redist.x64.exe'
    Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vc_redist.x64.exe' -OutFile $redist -UseBasicParsing
}
$sig = Get-AuthenticodeSignature -LiteralPath $redist
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft Corporation') { Fail "vc_redist.x64.exe is not signed by Microsoft ($($sig.Status))" }

$modSrc = Join-Path $Repo 'release\mod\moonshine_names'
if (-not (Test-Path -LiteralPath (Join-Path $modSrc 'mod.manifest'))) { Fail "no mod folder at $modSrc -- scripts/build_mod.sh release\mod\moonshine_names writes it" }
Mirror $modSrc (Join-Path $stage 'mod\moonshine_names')

# --- the check -----------------------------------------------------------------------------
# The package's own preflight, from inside the package, with the arguments the launcher
# will use. The game is on this machine, so the game root is found too.
Step 'preflight'
Push-Location $pkg
try {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'Moonshine.ps1') -Check
    if ($LASTEXITCODE -ne 0) { Fail 'the staged package does not pass its own preflight' }
} finally { Pop-Location }

$total = (Get-ChildItem -LiteralPath $pkg -Recurse -File | Measure-Object Length -Sum).Sum
Write-Output ("package: " + [math]::Round($total / 1GB, 2) + " GB in " + $pkg)

# --- the installer -------------------------------------------------------------------------
if ($SkipInstaller) { return }
Step 'installer'
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) {
    Write-Output '  installing Inno Setup (winget)'
    & winget install --id JRSoftware.InnoSetup --exact --silent --accept-package-agreements --accept-source-agreements | Out-Null
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $iscc) { Fail 'Inno Setup did not install; install it by hand from https://jrsoftware.org/isdl.php' }
}
$out = Join-Path $Repo "release\out\$Version"
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
Ensure-Dir $out
& $iscc "/DVersion=$Version" "/DStage=$stage" "/DSource=$installerSrc" "/O$out" (Join-Path $installerSrc 'moonshine.iss')
if ($LASTEXITCODE -ne 0) { Fail "ISCC failed ($LASTEXITCODE)" }
Get-ChildItem -LiteralPath $out | ForEach-Object { Write-Output ("  " + $_.Name + "  " + [math]::Round($_.Length / 1MB) + " MB") }
Write-Output "installer: $out"
