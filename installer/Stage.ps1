# Assemble the tester package on the Windows build machine, where the Windows build is,
# build the installer out of it with Inno Setup, and lay out what has to be uploaded.
#
#   powershell -ExecutionPolicy Bypass -File installer\Stage.ps1 -Version 0.9.94
#       -DownloadBase https://download.moonshine.ai/kcd2 [-SkipInstaller]
#
# Everything it packs is in this checkout: the release build (scripts\build_windows.bat
# release), the assembled drop (scripts\fetch_drop.ps1), the models (scripts\fetch_models.ps1)
# and the mod folder (scripts\build_mod.sh, or a copy of its output under release\mod). The
# tree it builds, under <Repo>\release\stage\, is the installed folder as it lands on a
# tester's disk, whole:
#
#   Moonshine\
#     moonshine_app.exe + DLLs      app\build-release\RelWithDebInfo\ (CPU-only build)
#     Moonshine.ps1, Fetch-Package.ps1, Collect-Logs.ps1, VERSION.txt, LICENSES\
#     README.txt                    the player's guide: README.md up to "## The repository"
#     setting.txt, manifest.json
#     data\kcd2\npcs.sqlite         a vacuumed copy of the assembled database...
#     data\kcd2\npcs-vectors.sqlite ...and of the knowledge base's vectors beside it
#     data\kcd2\voices\             phrases\ (the lines in voices this package has), voices.txt, facials.txt, lexicon.tsv
#     models\embeddinggemma-300m-q8_0.gguf
#     models\phonon-31m-en\         the voice: model, tokenizer, voices\*.safetensors
#     models\llm\<name>.gguf        the one GGUF under models\llm
#     models\stt\medium-streaming-en\ the one folder under models\stt
#     mod\moonshine_names\          from release\mod; Fetch-Package copies it into the game
#   extra\vc_redist.x64.exe         run by the installer, not installed
#
# The installer carries only the program, the scripts and manifest.json; the rest (data\,
# models\, mod\, about 3 GB) Setup downloads itself on the tester's machine, from [Files]
# entries this script writes to release\stage\download_files.iss out of the manifest: the
# models from where scripts\models.tsv pins them (Hugging Face, download.moonshine.ai), the
# database, voices and mod from <DownloadBase>/blobs/<sha256>/<name>. The files are named
# by their bytes, not by the version, so one that has not changed since the last release
# is the same URL and is never uploaded twice, and one that has changed lands beside the
# old one rather than over it: every installer ever handed out keeps finding exactly the
# bytes its manifest names. Fetch-Package.ps1 reads the same manifest afterwards, for
# repairs and for Moonshine.ps1 -Update. Under <Repo>\release\publish\<Version>\ go the
# manifest (with the installer's hash added) and the installer; installer\publish.py (or
# scripts/release.sh, which runs all of this from the repository) puts those and the
# blobs the manifest names up at <DownloadBase>, then <DownloadBase>/latest/manifest.json,
# which is what an installed Moonshine.ps1 -Update reads.
#
# The installer, its uninstaller and moonshine_app.exe are Authenticode-signed through Azure
# Artifact Signing when installer\signing.json is present (the account, certificate profile
# and endpoint; nothing secret) and the client tools are installed (winget install -e --id
# Microsoft.Azure.ArtifactSigningClientTools); the credential is this machine's `az login`.
# Without signing.json the build is unsigned, with a note; -NoSign forces that.
#
# -DownloadBase is MOONSHINE_DOWNLOAD_BASE from the environment when not given. Every copy
# is a robocopy or a hash-checked Copy-Item, so a second run only moves what changed; the
# big weights are the same bytes from one build to the next, and their sha256s are cached.
# -Repo is this checkout unless told otherwise; -Python, unless told, is the interpreter the
# py launcher names, else the first python.exe on the PATH.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$DownloadBase = $env:MOONSHINE_DOWNLOAD_BASE,
    [string]$Repo = '',
    [string]$Python = $env:PYTHON,
    [switch]$SkipInstaller,
    [switch]$NoSign
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has $PSScriptRoot empty while the parameter defaults are evaluated.
if (-not $Repo) { $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
if (-not $DownloadBase) { Write-Error 'where the database, voices and mod will be hosted: -DownloadBase https://... (or MOONSHINE_DOWNLOAD_BASE)'; exit 1 }
$DownloadBase = $DownloadBase.TrimEnd('/')
if (-not $Python) {
    # The py launcher's answer first: it names a real interpreter, where the python.exe on
    # the PATH may be the Store's stub or a uv shim whose interpreter has gone.
    $launcher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($launcher) {
        $found = & $launcher.Source -3 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $found -and (Test-Path -LiteralPath $found)) { $Python = $found }
    }
}
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
# UTF-8 without the byte-order mark Windows PowerShell's -Encoding UTF8 writes, which
# strict JSON readers (python, jq, a C++ parser) refuse.
function Write-Utf8([string]$path, [string]$text) { [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false)) }

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

# --- signing -------------------------------------------------------------------------------
# One command for everything that gets a signature: signtool with the Artifact Signing dlib
# and the metadata file, timestamped by Microsoft's authority (the certificates live three
# days; the timestamp is what keeps the signature valid after). Written to stage\sign.cmd so
# that ISCC can be given the same command for setup.exe and the uninstaller without the
# quoting of two shells in between.
$signCmd = $null
$signMeta = Join-Path $installerSrc 'signing.json'
if ($NoSign) {
    Write-Output '  not signing (-NoSign)'
} elseif (-not (Test-Path -LiteralPath $signMeta)) {
    Write-Output '  not signing: no installer\signing.json (the Artifact Signing account and certificate profile)'
} else {
    $toolsDir = @("$env:ProgramFiles\Artifact Signing Client Tools", "$env:ProgramFiles\Trusted Signing Client Tools") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $toolsDir) { Fail 'installer\signing.json is here but the signing tools are not: winget install -e --id Microsoft.Azure.ArtifactSigningClientTools (or build with -NoSign)' }
    $signtool = @(Get-ChildItem -LiteralPath $toolsDir -Recurse -Filter 'signtool.exe' | Where-Object { $_.FullName -notmatch '\\(x86|arm64)\\' } | Select-Object -First 1)
    $dlib = @(Get-ChildItem -LiteralPath $toolsDir -Recurse -Filter '*.Dlib.dll' | Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1)
    if ($signtool.Count -eq 0 -or $dlib.Count -eq 0) { Fail "the signing tools under $toolsDir have no signtool.exe or x64 dlib" }
    $signCmd = Join-Path $stage 'sign.cmd'
    Set-Content -LiteralPath $signCmd -Encoding ASCII -Value @(
        '@echo off',
        ('"{0}" sign /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib "{1}" /dmdf "{2}" %*' -f $signtool[0].FullName, $dlib[0].FullName, $signMeta)
    )
    Write-Output "  signing with $($signtool[0].FullName)"
}
function Sign-File([string]$path) {
    if (-not $signCmd) { return }
    if ((Get-AuthenticodeSignature -LiteralPath $path).Status -eq 'Valid') { return }
    & $signCmd $path 2>&1 | Where-Object { "$_" -match 'rror|Success' } | ForEach-Object { Write-Output "    $_" }
    if ($LASTEXITCODE -ne 0) { Fail "signing $path failed ($LASTEXITCODE); is `az login` current on this machine?" }
    $sig = Get-AuthenticodeSignature -LiteralPath $path
    if ($sig.Status -ne 'Valid') { Fail "$path was signed but the signature is $($sig.Status)" }
    Write-Output ("  signed " + (Split-Path -Leaf $path) + " as " + $sig.SignerCertificate.Subject)
}

# --- the program ---------------------------------------------------------------------------
Step 'program'
$exe = Join-Path $build 'moonshine_app.exe'
if (-not (Test-Path -LiteralPath $exe)) { Fail "no release build at $exe -- run scripts\build_windows.bat release" }
$dlls = @(Get-ChildItem -LiteralPath $build -Filter '*.dll')
$cuda = @($dlls | Where-Object { $_.Name -match '^(cublas|cudart|cublasLt|ggml-cuda)' })
if ($cuda.Count -gt 0) { Fail ("the release build has CUDA DLLs beside it: " + (($cuda | ForEach-Object Name) -join ', ') + " -- that is not the release build") }
foreach ($need in @('ptts_ffi.dll', 'moonshine.dll', 'onnxruntime.dll')) {
    if (-not ($dlls | Where-Object Name -eq $need)) { Fail "the release build is missing $need" }
}
Copy-Same $exe (Join-Path $pkg 'moonshine_app.exe')
Sign-File (Join-Path $pkg 'moonshine_app.exe')
foreach ($dll in $dlls) { Copy-Same $dll.FullName (Join-Path $pkg $dll.Name) }
# Anything else left in the package root from an earlier build.
Get-ChildItem -LiteralPath $pkg -File | Where-Object { $_.Extension -in '.dll', '.exe', '.pdb' -and -not (Test-Path -LiteralPath (Join-Path $build $_.Name)) } |
    ForEach-Object { Write-Output ("  removing " + $_.Name); Remove-Item -LiteralPath $_.FullName -Force }

foreach ($name in @('Moonshine.ps1', 'Fetch-Package.ps1', 'Collect-Logs.ps1')) {
    Copy-Same (Join-Path $installerSrc $name) (Join-Path $pkg $name)
}
# README.txt is the player's guide: README.md up to its first developer heading, "## The
# repository" (the installer shows it before installing and from the Start Menu). Written
# only when it differs, so the stamp-and-size copies stay quiet.
$readmeLines = Get-Content -LiteralPath (Join-Path $Repo 'README.md')
$cutAt = [array]::IndexOf($readmeLines, '## The repository')
if ($cutAt -lt 0) { Fail 'README.md has no "## The repository" heading to cut the player''s guide at' }
$readme = ($readmeLines[0..($cutAt - 1)] -join "`r`n").TrimEnd() + "`r`n"
$readmeOut = Join-Path $pkg 'README.txt'
if (-not (Test-Path -LiteralPath $readmeOut) -or ((Get-Content -LiteralPath $readmeOut -Raw) -ne $readme)) {
    [IO.File]::WriteAllText($readmeOut, $readme, (New-Object Text.UTF8Encoding $false))
}
Mirror (Join-Path $installerSrc 'LICENSES') (Join-Path $pkg 'LICENSES')
Set-Content -LiteralPath (Join-Path $pkg 'VERSION.txt') -Value $Version -NoNewline
Copy-Same (Join-Path $Repo 'setting.txt') (Join-Path $pkg 'setting.txt')

# --- the database --------------------------------------------------------------------------
# Two files (data\schema.sql): npcs.sqlite, and npcs-vectors.sqlite beside it with the
# knowledge base's vectors, which the app attaches. The assembled pair (scripts\fetch_drop.ps1)
# is already clean; VACUUM INTO copies each from a consistent read all the same, so a DB the
# app has open, or one somebody edited by hand, ships whole. 16 KB pages, as the assembler
# writes them: the rows are long and a bigger page is fewer reads and less slack.
Step 'database'
$dbDir = Join-Path $Repo 'data\kcd2'
foreach ($name in @('npcs.sqlite', 'npcs-vectors.sqlite')) {
    $db = Join-Path $dbDir $name
    if (-not (Test-Path -LiteralPath $db)) { Fail "no database at $db -- run scripts\fetch_drop.ps1" }
    $dbOut = Join-Path $pkg ('data\kcd2\' + $name)
    Ensure-Dir (Split-Path -Parent $dbOut)
    $stampFile = "$dbOut.from"
    $srcStamp = (Get-Item -LiteralPath $db).LastWriteTimeUtc.Ticks.ToString()
    if ((Test-Path -LiteralPath $dbOut) -and (Test-Path -LiteralPath $stampFile) -and ((Get-Content -LiteralPath $stampFile) -eq $srcStamp)) {
        Write-Output "  $name unchanged"
        continue
    }
    if (Test-Path -LiteralPath $dbOut) { Remove-Item -LiteralPath $dbOut -Force }
    $py = @"
import sqlite3, sys
src, dst = sys.argv[1], sys.argv[2]
con = sqlite3.connect(src)
con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
con.execute("PRAGMA page_size=16384")
con.execute("VACUUM INTO ?", (dst,))
con.close()
chk = sqlite3.connect(dst)
print("  integrity:", chk.execute("PRAGMA integrity_check").fetchone()[0])
print("  tables:", chk.execute("select count(*) from sqlite_master where type='table'").fetchone()[0],
      " page size:", chk.execute("PRAGMA page_size").fetchone()[0])
chk.close()
"@
    $pyFile = Join-Path $env:TEMP 'moonshine_vacuum.py'
    Set-Content -LiteralPath $pyFile -Value $py
    Write-Output "  $name"
    & $Python $pyFile $db $dbOut
    if ($LASTEXITCODE -ne 0) { Fail "VACUUM INTO failed for $name" }
    Remove-Item -LiteralPath $pyFile -Force
    Set-Content -LiteralPath $stampFile -Value $srcStamp -NoNewline
    Write-Output ("  " + [math]::Round((Get-Item -LiteralPath $dbOut).Length / 1MB) + " MB")
}

# --- the voices ----------------------------------------------------------------------------
Step 'voices'
$voices = Join-Path $Repo 'data\kcd2\voices'
$voicesOut = Join-Path $pkg 'data\kcd2\voices'
# The voices themselves are in the Phonon model folder (models\phonon-31m-en\voices), below.
# The clones\ folder of .qvoice recordings was the Qwen3-TTS engine's and is not shipped;
# one left in the stage by an earlier build goes.
if (Test-Path -LiteralPath (Join-Path $voicesOut 'clones')) { Remove-Item -LiteralPath (Join-Path $voicesOut 'clones') -Recurse -Force }
# The lines spoken ahead (phrases\index.tsv: hash, voice, words; the audio beside it as
# <hash>.opus), but only those in a voice this package carries. A line baked in a voice
# that is gone -- the drop's are still in the Qwen3-TTS recordings, pete-male and the
# rest -- would never be asked for and would be heard in the wrong voice if it were, so
# it stays behind; the folder ships with its index and whatever lines pass, or empty.
$phrasesIn = Join-Path $voices 'phrases'
$phrasesOut = Join-Path $voicesOut 'phrases'
$voiceNames = @(Get-ChildItem -LiteralPath (Join-Path $Repo 'models\phonon-31m-en\voices') -Filter '*.safetensors' -ErrorAction SilentlyContinue | ForEach-Object BaseName)
if (Test-Path -LiteralPath $phrasesOut) { Remove-Item -LiteralPath $phrasesOut -Recurse -Force }
Ensure-Dir $phrasesOut
$kept = New-Object System.Collections.Generic.List[string]
$kept.Add('# hash, actor, words. Opus at 16 kbit/s beside this file, one line each.')
$dropped = 0
if (Test-Path -LiteralPath (Join-Path $phrasesIn 'index.tsv')) {
    foreach ($line in Get-Content -LiteralPath (Join-Path $phrasesIn 'index.tsv')) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $cols = $line.Split("`t")
        if ($cols.Count -lt 3) { continue }
        $opus = Join-Path $phrasesIn ($cols[0] + '.opus')
        if ($voiceNames -contains $cols[1] -and (Test-Path -LiteralPath $opus)) {
            Copy-Same $opus (Join-Path $phrasesOut ($cols[0] + '.opus'))
            $kept.Add($line)
        } else {
            $dropped++
        }
    }
}
Write-Utf8 (Join-Path $phrasesOut 'index.tsv') (($kept -join "`n") + "`n")
Write-Output ("  phrases: " + ($kept.Count - 1) + " line(s) spoken ahead" + $(if ($dropped) { ", $dropped left behind (not in a voice this package carries)" } else { '' }))
foreach ($name in @('facials.txt', 'lexicon.tsv')) {
    Copy-Same (Join-Path $voices $name) (Join-Path $voicesOut $name)
}
# voices.txt names the Kokoro voices, which this build does not carry; the app reads it
# only when it is there. Shipped when the checkout has it, so the catalogue is complete.
if (Test-Path -LiteralPath (Join-Path $voices 'voices.txt')) { Copy-Same (Join-Path $voices 'voices.txt') (Join-Path $voicesOut 'voices.txt') }

# --- the models ----------------------------------------------------------------------------
# All four come from scripts\fetch_models.ps1 (scripts\models.tsv pins them), which is the
# check that they are the right bytes; this only insists that they are there.
Step 'models'
$models = Join-Path $Repo 'models'
$modelsOut = Join-Path $pkg 'models'
Copy-Same (Join-Path $models 'embeddinggemma-300m-q8_0.gguf') (Join-Path $modelsOut 'embeddinggemma-300m-q8_0.gguf')
# The voice: Phonon's checkpoint with the souls' voices inside it. The Qwen3-TTS folder an
# earlier stage left behind goes.
Mirror (Join-Path $models 'phonon-31m-en') (Join-Path $modelsOut 'phonon-31m-en') @('.cache', '.git') @('.gitattributes', '*.md')
if (-not (Test-Path -LiteralPath (Join-Path $modelsOut 'phonon-31m-en\model.q8.gguf'))) { Fail 'phonon-31m-en has no model.q8.gguf -- scripts\fetch_models.ps1' }
$phononVoices = @(Get-ChildItem -LiteralPath (Join-Path $modelsOut 'phonon-31m-en\voices') -Filter '*.safetensors' -ErrorAction SilentlyContinue)
if ($phononVoices.Count -eq 0) { Fail 'phonon-31m-en\voices holds no .safetensors -- scripts\fetch_models.ps1' }
Write-Output ("  voice: phonon-31m-en, " + $phononVoices.Count + " voices")
if (Test-Path -LiteralPath (Join-Path $modelsOut 'qwen3-tts-0.6b')) { Write-Output '  removing qwen3-tts-0.6b'; Remove-Item -LiteralPath (Join-Path $modelsOut 'qwen3-tts-0.6b') -Recurse -Force }

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

# --- the mod -------------------------------------------------------------------------------
# Inside the package, under mod\, which is where Fetch-Package.ps1 copies it into the game
# from; it is downloaded with the rest rather than carried by the installer.
Step 'mod'
$modSrc = Join-Path $Repo 'release\mod\moonshine_names'
if (-not (Test-Path -LiteralPath (Join-Path $modSrc 'mod.manifest'))) { Fail "no mod folder at $modSrc -- scripts/build_mod.sh release\mod\moonshine_names writes it" }
Mirror $modSrc (Join-Path $pkg 'mod\moonshine_names')
# Where an earlier Stage.ps1 put it.
if (Test-Path -LiteralPath (Join-Path $stage 'mod')) { Remove-Item -LiteralPath (Join-Path $stage 'mod') -Recurse -Force }
# The mod's manifest says which build it is, and release\mod was built whenever it was
# (scripts/build_mod.sh stamps it with the version of that day). The staged copy is
# stamped with this release's: its <version>, and the "tester build N" its description
# names, so the mod a tester sees in the game's mod list and the app beside it agree.
# Only the manifest; the paks are the same bytes and keep their URLs.
$modManifest = Join-Path $pkg 'mod\moonshine_names\mod.manifest'
$modXml = [IO.File]::ReadAllText($modManifest)
$stamped = $modXml -replace '<version>[^<]*</version>', "<version>$Version</version>" -replace 'tester build \d+(\.\d+)+', "tester build $Version"
if ($stamped -ne $modXml) {
    Write-Utf8 $modManifest $stamped
    Write-Output "  mod.manifest stamped $Version"
}

# --- the manifest --------------------------------------------------------------------------
# Every file under data\, models\ and mod\: path, size, sha256, and where Fetch-Package.ps1
# gets it. The models' URLs are the pins in scripts\models.tsv (and the staged bytes must
# match the pins); everything else is <DownloadBase>/blobs/<sha256>/<name>, the file's own
# hash, so a release re-uses what an earlier one uploaded and never overwrites it (the
# name on the end is for whoever reads a log or a bucket listing; the hash is the
# address). Hashing 8 GB is a minute, so hashes are kept beside the stage, keyed by size
# and time.
Step 'manifest'
$hashCache = Join-Path $stage 'sha256-cache.json'
$cache = @{}
if (Test-Path -LiteralPath $hashCache) {
    $o = Get-Content -LiteralPath $hashCache -Raw | ConvertFrom-Json
    foreach ($p in $o.PSObject.Properties) { $cache[$p.Name] = $p.Value }
}
function Sha256-Cached([string]$path, [string]$rel) {
    $item = Get-Item -LiteralPath $path
    $key = "$($item.Length):$($item.LastWriteTimeUtc.Ticks)"
    $hit = $cache[$rel]
    if ($hit -and $hit.key -eq $key) { return $hit.sha256 }
    $h = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $cache[$rel] = [pscustomobject]@{ key = $key; sha256 = $h }
    return $h
}
$pins = @{}   # models/<dest> -> @{ url; bytes; sha256 }
foreach ($line in Get-Content -LiteralPath (Join-Path $Repo 'scripts\models.tsv')) {
    if ($line.Trim() -eq '' -or $line.StartsWith('#')) { continue }
    $cols = $line.Split("`t")
    if ($cols.Count -lt 4) { continue }
    $pins['models/' + $cols[0].Trim()] = @{ url = $cols[1].Trim(); bytes = [long]$cols[2].Trim(); sha256 = $cols[3].Trim().ToLowerInvariant() }
}
$entries = New-Object System.Collections.ArrayList
$payload = 0L
foreach ($top in @('data', 'mod', 'models')) {
    $files = Get-ChildItem -LiteralPath (Join-Path $pkg $top) -Recurse -File -Force | Where-Object { $_.Name -notlike '*.from' -and $_.Name -notlike '*.part' } | Sort-Object FullName
    foreach ($file in $files) {
        $rel = $file.FullName.Substring($pkg.Length + 1) -replace '\\', '/'
        if ($top -eq 'models') {
            # Only what models.tsv pins can be downloaded; a model folder cloned from the Hub
            # brings a README and .gitattributes the app never reads, and they go.
            $pin = $pins[$rel]
            if (-not $pin) {
                Write-Output "  removing $rel (not pinned in scripts\models.tsv)"
                Remove-Item -LiteralPath $file.FullName -Force
                continue
            }
            $sha = Sha256-Cached $file.FullName $rel
            if ($pin.bytes -ne $file.Length -or $pin.sha256 -ne $sha) { Fail "$rel differs from its pin in scripts\models.tsv (run scripts\fetch_models.ps1)" }
            $url = $pin.url
        } else {
            $sha = Sha256-Cached $file.FullName $rel
            $url = $DownloadBase + '/blobs/' + $sha + '/' + [Uri]::EscapeDataString($file.Name)
        }
        [void]$entries.Add([ordered]@{ path = $rel; bytes = $file.Length; sha256 = $sha; url = $url })
        $payload += $file.Length
    }
}
foreach ($rel in $pins.Keys) {
    if (-not ($entries | Where-Object { $_.path -eq $rel })) { Write-Output "  note: $rel is pinned in models.tsv but not staged (not shipped)" }
}
$cacheOut = [ordered]@{}
foreach ($k in ($cache.Keys | Sort-Object)) { $cacheOut[$k] = $cache[$k] }
Write-Utf8 $hashCache ($cacheOut | ConvertTo-Json -Depth 3)
$setupName = "Moonshine-$Version-setup.exe"
$manifest = [ordered]@{
    version = $Version
    latest  = "$DownloadBase/latest/manifest.json"
    setup   = [ordered]@{ url = "$DownloadBase/$Version/$setupName" }
    files   = @($entries)
}
$manifestPath = Join-Path $pkg 'manifest.json'
Write-Utf8 $manifestPath ($manifest | ConvertTo-Json -Depth 4)
Write-Output ("  " + $entries.Count + " files, " + [math]::Round($payload / 1GB, 2) + " GB to download; models from their pins, the rest from $DownloadBase/blobs/")

# The same list as [Files] entries for moonshine.iss, which includes this file: Setup
# downloads each file itself, inside its own progress bar, and checks the sha256. Two
# entries a file: a copy from /MIRROR=<folder> when that folder has the right bytes (the
# verify scripts, on the machine that has the stage), else the download. The Check
# functions are in moonshine.iss. Only the download entry carries ExternalSize, so the
# free-space total counts each file once. Then the mod's files, from where they were
# downloaded into the game's mods folder, for the copymod task.
Step 'download list'
$iss = New-Object System.Collections.Generic.List[string]
$iss.Add('; Written by installer\Stage.ps1 from manifest.json; do not edit. Included by moonshine.iss.')
$iss.Add('[Files]')
foreach ($e in $entries) {
    $win = $e.path -replace '/', '\'
    $dir = Split-Path -Parent $win
    $name = Split-Path -Leaf $win
    $destDir = if ($dir) { '{app}\' + $dir } else { '{app}' }
    $iss.Add(('Source: "{{param:MIRROR}}\{0}"; DestDir: "{1}"; Hash: "{2}"; Flags: external ignoreversion; Check: FromMirror(''{0}'', ''{3}'', ''{2}'')' -f $win, $destDir, $e.sha256, $e.bytes))
    $iss.Add(('Source: "{0}"; DestDir: "{1}"; DestName: "{2}"; ExternalSize: {3}; Hash: "{4}"; Flags: external download ignoreversion; Check: FromWeb(''{5}'', ''{3}'', ''{4}'')' -f $e.url, $destDir, $name, $e.bytes, $e.sha256, $win))
}
$iss.Add('; The mod, into the game, from the copy above.')
foreach ($e in ($entries | Where-Object { $_.path -like 'mod/moonshine_names/*' })) {
    $win = $e.path -replace '/', '\'
    $sub = Split-Path -Parent ($win.Substring('mod\moonshine_names\'.Length))
    $destDir = '{code:GameDir}\mods\moonshine_names' + $(if ($sub) { '\' + $sub } else { '' })
    $iss.Add(('Source: "{{app}}\{0}"; DestDir: "{1}"; ExternalSize: {2}; Flags: external ignoreversion uninsneveruninstall; Tasks: copymod' -f $win, $destDir, $e.bytes))
}
$issPath = Join-Path $stage 'download_files.iss'
Set-Content -LiteralPath $issPath -Value ($iss -join "`r`n") -Encoding ASCII
Write-Output "  $issPath"

# --- the check -----------------------------------------------------------------------------
# The package's own preflight, from inside the package, with the arguments the launcher
# will use. The game is on this machine, so the game root is found too. Then the manifest
# against the files it describes, as Fetch-Package.ps1 will read them on a tester's machine.
Step 'preflight'
Push-Location $pkg
try {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'Moonshine.ps1') -Check
    if ($LASTEXITCODE -ne 0) { Fail 'the staged package does not pass its own preflight' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'Fetch-Package.ps1') -Check | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { Fail 'the manifest does not describe the staged files' }
    Remove-Item -LiteralPath (Join-Path $pkg 'logs') -Recurse -Force -ErrorAction SilentlyContinue
} finally { Pop-Location }

$total = (Get-ChildItem -LiteralPath $pkg -Recurse -File | Measure-Object Length -Sum).Sum
Write-Output ("package: " + [math]::Round($total / 1GB, 2) + " GB in " + $pkg)

# --- what goes up --------------------------------------------------------------------------
# The manifest, and the installer once built, with its hash in the published manifest (the
# one inside the installer cannot carry its own hash). The files the manifest points at
# are not copied here: publish.py reads them out of the stage, by the paths and hashes the
# manifest gives, and uploads only those the bucket does not have.
Step 'publish tree'
$publish = Join-Path $Repo "release\publish\$Version"
Ensure-Dir $publish
# The copies of data\ and mod\ an earlier Stage.ps1 (to 0.9.97) laid out here for upload.
foreach ($top in @('data', 'mod')) {
    if (Test-Path -LiteralPath (Join-Path $publish $top)) { Write-Output "  removing $top\ (served from blobs\ now)"; Remove-Item -LiteralPath (Join-Path $publish $top) -Recurse -Force }
}
if (Test-Path -LiteralPath (Join-Path $Repo 'release\publish\latest')) { Remove-Item -LiteralPath (Join-Path $Repo 'release\publish\latest') -Recurse -Force }
Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $publish 'manifest.json') -Force
Write-Output "  $publish"

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
# With signing, ISCC is told the command as "azure" (moonshine.iss names it in SignTool=)
# and signs setup.exe and the uninstaller with it. The command line is written to a .cmd
# and run from there: quotes do not survive PowerShell 5.1's hand-off to a native program.
$isccLine = "`"$iscc`" /DVersion=$Version `"/DStage=$stage`" `"/DSource=$installerSrc`" `"/O$out`""
if ($signCmd) { $isccLine += " /DSign `"/Sazure=$signCmd `$f`"" }
$isccLine += " `"$(Join-Path $installerSrc 'moonshine.iss')`""
$isccCmd = Join-Path $stage 'iscc.cmd'
Set-Content -LiteralPath $isccCmd -Encoding ASCII -Value @('@echo off', $isccLine, 'exit /b %ERRORLEVEL%')
& cmd /c $isccCmd
if ($LASTEXITCODE -ne 0) { Fail "ISCC failed ($LASTEXITCODE)" }
Get-ChildItem -LiteralPath $out | ForEach-Object { Write-Output ("  " + $_.Name + "  " + [math]::Round($_.Length / 1MB) + " MB") }
if ($signCmd) {
    $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $out "Moonshine-$Version-setup.exe")
    if ($sig.Status -ne 'Valid') { Fail "the installer's signature is $($sig.Status)" }
    Write-Output ("  signed as " + $sig.SignerCertificate.Subject)
}
Write-Output "installer: $out"

# The installer into the publish tree, and its hash into the published manifest. The same
# bytes become <DownloadBase>/latest/manifest.json when publish.py has put up everything
# they name; that is what an installed Moonshine.ps1 -Update compares itself against.
$setupOut = Join-Path $out $setupName
if (-not (Test-Path -LiteralPath $setupOut)) { Fail "ISCC did not write $setupOut" }
Copy-Same $setupOut (Join-Path $publish $setupName)
$manifest.setup = [ordered]@{
    url    = $manifest.setup.url
    bytes  = (Get-Item -LiteralPath $setupOut).Length
    sha256 = (Get-FileHash -LiteralPath $setupOut -Algorithm SHA256).Hash.ToLowerInvariant()
}
Write-Utf8 (Join-Path $publish 'manifest.json') ($manifest | ConvertTo-Json -Depth 4)
Write-Output "publish: py -3 installer\publish.py --version $Version   (the R2 credentials in the environment, or scripts/release.sh from the repository)"
