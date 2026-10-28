# The published installer on a machine, as its own user, as a tester would run it: fetched
# through the testers' signed links, installed silently, checked, and talked to. On a
# rented machine that is a different Windows, a different CPU, and a VC++ runtime the
# installer may have to bring itself; on a development laptop it is the real Steam game
# and the saves a player has.
#
#   powershell -ExecutionPolicy Bypass -File installer\Verify-Install.ps1 -Version 0.9.94 -Links D:\links.txt
#       [-Game C:\KCD2] [-Python 'C:\Program Files\Python312\python.exe'] [-FakeGame D:\fake_game.py]
#       [-Launcher D:\Moonshine.ps1] [-Seed C:\path\to\release\out\0.9.94] [-Mirror C:\path\to\release\stage\Moonshine]
#       [-KeepApp]
#
# The links file is the one the publisher wrote for testers: the installer's URL, followed
# by its SHA-256 (the installer downloads the 8 GB of models and data itself). Steps:
#
#   download   the installer, with curl, unless the one here already has the SHA-256 the
#              file lists; -Seed names a folder whose copy is tried first
#   install    any earlier Moonshine removed first; the runtime's presence noted before and
#              after, since on a fresh image the installer is what brings it. The installer
#              downloads the models and data as part of this, the real test of the published
#              URLs; -Mirror names a folder laid out like the installed one (the stage on the
#              machine that built it) and is passed as /MIRROR, so files with the right hash
#              are copied from there instead of fetched
#   cfg        the cloud machines carry the GOG build at C:\KCD2, which neither Steam's
#              registry nor the app's fixed paths know, so `root = <game>` goes into
#              moonshine.cfg -- the road a tester with a second Steam library takes
#   check      Moonshine.ps1 -Check must end in `ready`
#   converse   scripts/fake_game.py against the installed tree: model, embedder, voice
#   app        unless -KeepApp is off: the launcher is left running with -NoGame, from the
#              installed folder, so a real game started after it (the autopilot) finds the
#              app following its log. -Launcher overlays a newer Moonshine.ps1 first, for a
#              switch the shipped one does not have yet.
#
# The uninstall is the caller's last step, after the in-game part.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Links,
    [string]$Game = 'C:\KCD2',
    [string]$Python = 'C:\Program Files\Python312\python.exe',
    [string]$FakeGame = '',
    [string]$Launcher = '',
    [string]$Seed = '',
    [string]$Mirror = '',
    [switch]$KeepApp = $true
)
$ErrorActionPreference = 'Stop'
$faults = @()
function Step([string]$t) { Write-Output ("=== " + $t) }
function Fault([string]$t) { $script:faults += $t; Write-Output ("  FAULT: " + $t) }
function Runtime-Present {
    $k = 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64'
    if (-not (Test-Path $k)) { return 'absent' }
    $v = Get-ItemProperty $k
    if ($v.Installed -ne 1) { return 'absent' }
    return "$($v.Major).$($v.Minor).$($v.Bld)"
}

$work = Join-Path $env:USERPROFILE "moonshine-verify\$Version"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$app = Join-Path $env:LOCALAPPDATA 'Moonshine'

# --- download ------------------------------------------------------------------------------
Step "download, from $Links"
$lines = Get-Content -LiteralPath $Links
$want = @{}   # name -> url
$sha = @{}    # name -> hash
for ($i = 0; $i -lt $lines.Count; $i++) {
    $l = $lines[$i]
    if ($l -match '^(Moonshine-[^\s]+)$') {
        $name = $Matches[1]
        if ($i + 1 -lt $lines.Count -and $lines[$i + 1] -match '^\s+(https://\S+)') { $want[$name] = $Matches[1] }
    }
    if ($l -match '^\s+([0-9A-F]{64})\s+(Moonshine-\S+)$') { $sha[$Matches[2]] = $Matches[1] }
}
if ($want.Count -eq 0) { Write-Error "no links in $Links"; exit 1 }
foreach ($name in ($want.Keys | Sort-Object)) {
    $dest = Join-Path $work $name
    if ($Seed -and -not (Test-Path -LiteralPath $dest) -and (Test-Path -LiteralPath (Join-Path $Seed $name))) {
        Copy-Item -LiteralPath (Join-Path $Seed $name) -Destination $dest
        Write-Output "  $name from $Seed"
    }
    if ((Test-Path -LiteralPath $dest) -and $sha[$name] -and ((Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash -eq $sha[$name])) {
        Write-Output "  $name already here, hash matches"; continue
    }
    $t0 = Get-Date
    & curl.exe --fail --silent --show-error --location -o $dest $want[$name]
    if ($LASTEXITCODE -ne 0) { Fault "download of $name failed (curl $LASTEXITCODE)"; continue }
    $mb = [math]::Round((Get-Item -LiteralPath $dest).Length / 1MB)
    $s = [math]::Round(((Get-Date) - $t0).TotalSeconds)
    $ok = if ($sha[$name]) { if ((Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash -eq $sha[$name]) { 'hash ok' } else { 'HASH MISMATCH' } } else { 'no hash to check' }
    if ($ok -eq 'HASH MISMATCH') { Fault "$name does not match its SHA-256" }
    Write-Output "  $name  $mb MB in ${s}s, $ok"
}
$setup = Join-Path $work "Moonshine-$Version-setup.exe"
if (-not (Test-Path -LiteralPath $setup)) { Write-Error "no $setup"; exit 1 }

# --- install ---------------------------------------------------------------------------------
Step 'install'
Get-Process moonshine_app -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath (Join-Path $app 'unins000.exe')) {
    Write-Output '  removing the earlier Moonshine'
    Start-Process -FilePath (Join-Path $app 'unins000.exe') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
}
if (Test-Path -LiteralPath $app) { Remove-Item -LiteralPath $app -Recurse -Force -ErrorAction SilentlyContinue }
Write-Output ("  VC++ runtime before: " + (Runtime-Present))
$installLog = Join-Path $work 'install.log'
$t0 = Get-Date
$setupArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/MERGETASKS=!copymod', "/LOG=`"$installLog`"")
if ($Mirror) { $setupArgs += "/MIRROR=`"$Mirror`"" }
$p = Start-Process -FilePath $setup -ArgumentList $setupArgs -Wait -PassThru
Write-Output ("  installer exit " + $p.ExitCode + " after " + [math]::Round(((Get-Date) - $t0).TotalSeconds) + "s")
if ($p.ExitCode -ne 0) { Fault "the installer exited $($p.ExitCode)" }
# How the big files came: the [Code] logs a line per file each time Setup asks its Check.
function Count-Logged([string]$tag) {
    return @(Select-String -LiteralPath $installLog -Pattern $tag -ErrorAction SilentlyContinue | ForEach-Object { $_.Line -replace "^.*$tag" } | Sort-Object -Unique).Count
}
Write-Output ("  files: {0} downloaded, {1} from the mirror, {2} already right" -f (Count-Logged 'to download: '), (Count-Logged 'from the mirror: '), (Count-Logged 'already right: '))
Write-Output ("  VC++ runtime after:  " + (Runtime-Present))
if ((Runtime-Present) -eq 'absent') { Fault 'no VC++ runtime after the install' }
# The [Code] logs 'VC++ runtime already present' when its Check says no; a run shows as
# a 'Running the command' line naming the exe.
$skipped = Select-String -LiteralPath $installLog -Pattern 'VC\+\+ runtime already present' -Quiet
$ran = Select-String -LiteralPath $installLog -Pattern 'Running the command.*vc_redist' -Quiet
Write-Output ("  installer log: vc_redist " + $(if ($ran) { 'was run' } elseif ($skipped) { 'skipped, runtime already present' } else { 'neither run nor skipped?' }))
foreach ($need in @('moonshine_app.exe', 'Moonshine.ps1', 'Fetch-Package.ps1', 'Collect-Logs.ps1', 'README.txt', 'VERSION.txt', 'setting.txt', 'manifest.json', 'LICENSES\README.txt',
                    'data\kcd2\npcs.sqlite', 'models\embeddinggemma-300m-q8_0.gguf', 'models\qwen3-tts-0.6b\model.safetensors',
                    'libopenblas.dll', 'moonshine.dll', 'onnxruntime.dll', 'unins000.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $app $need))) { Fault "installed tree lacks $need" }
}
$llm = @(Get-ChildItem -LiteralPath (Join-Path $app 'models\llm') -Filter '*.gguf' -ErrorAction SilentlyContinue)
if ($llm.Count -ne 1) { Fault "expected one LLM gguf, found $($llm.Count)" }
$stt = @(Get-ChildItem -LiteralPath (Join-Path $app 'models\stt') -Directory -ErrorAction SilentlyContinue)
if ($stt.Count -ne 1) { Fault "expected one STT folder, found $($stt.Count)" }
$installed = (Get-ChildItem -LiteralPath $app -Recurse -File | Measure-Object Length -Sum).Sum
Write-Output ("  installed: " + [math]::Round($installed / 1GB, 2) + " GB")
if ($faults.Count -gt 0) { Write-Output ("FAULTS: " + ($faults -join '; ')); exit 1 }

# --- cfg -------------------------------------------------------------------------------------
Step 'cfg'
$cfg = Join-Path $app 'moonshine.cfg'
$had = if (Test-Path -LiteralPath $cfg) { (Get-Content -LiteralPath $cfg) -join ' | ' } else { '(none)' }
Write-Output "  as installed: $had"
if ($had -match 'root\s*=') { Write-Output '  the installer found a game through Steam; overriding with the GOG one for this test' }
$kept = @()
if (Test-Path -LiteralPath $cfg) { $kept = @(Get-Content -LiteralPath $cfg | Where-Object { $_ -notmatch '^\s*root\s*=' }) }
Set-Content -LiteralPath $cfg -Value ($kept + @("root = $Game"))
if ($Launcher -and (Test-Path -LiteralPath $Launcher)) {
    Copy-Item -LiteralPath $Launcher -Destination (Join-Path $app 'Moonshine.ps1') -Force
    Write-Output "  Moonshine.ps1 overlaid from $Launcher"
}

# --- check -----------------------------------------------------------------------------------
Step 'check'
Push-Location $app
try {
    $checkLog = Join-Path $work 'check.log'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $app 'Moonshine.ps1') -Check 2>&1 | Tee-Object -FilePath $checkLog | ForEach-Object { "  " + $_ }
    if ($LASTEXITCODE -ne 0) { Fault "Moonshine.ps1 -Check exited $LASTEXITCODE" }
    $text = Get-Content -LiteralPath $checkLog -Raw
    if ($text -notmatch '(?m)^ready') { Fault 'the check did not end in ready' }
} finally { Pop-Location }
if ($faults.Count -gt 0) { Write-Output ("FAULTS: " + ($faults -join '; ')); exit 1 }

# --- converse --------------------------------------------------------------------------------
if ($FakeGame -and (Test-Path -LiteralPath $FakeGame)) {
    Step 'conversation (fake game)'
    Push-Location $app
    try {
        $env:MOONSHINE_TTS_BACKEND = 'cpu'
        $convLog = Join-Path $work 'conversation.log'
        & $Python $FakeGame --app (Join-Path $app 'moonshine_app.exe') --world setting.txt --gguf ('models\llm\' + $llm[0].Name) `
            --personas 'data/kcd2/npcs.sqlite' --warmup 300 --timeout 180 2>&1 | Tee-Object -FilePath $convLog |
            Where-Object { $_ -notmatch '^\s+(row \d|player:|@said|@voiced)' } | ForEach-Object { "  " + $_ }
        if ($LASTEXITCODE -ne 0) { Fault "fake_game.py exited $LASTEXITCODE" }
    } finally { Pop-Location }
} else {
    Step 'conversation skipped (no fake_game.py given)'
}

# --- the app, left running for the real game ---------------------------------------------------
if ($KeepApp) {
    Step 'app'
    Get-Process moonshine_app -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    # Detached from this ssh session (Win32_Process.Create, as the development launcher does), the
    # launcher's own supervised loop, with the game's start left to the autopilot.
    $appLog = Join-Path $work 'launcher.log'
    Remove-Item -LiteralPath $appLog -ErrorAction SilentlyContinue
    $line = "cmd /c cd /d `"$app`" && powershell -NoProfile -ExecutionPolicy Bypass -File `"$app\Moonshine.ps1`" -NoGame > `"$appLog`" 2>&1"
    $r = ([WMICLASS]"Win32_Process").Create($line)
    if ($r.ReturnValue -ne 0) { Fault "could not start the launcher (Create $($r.ReturnValue))" }
    else {
        $up = $false
        for ($i = 0; $i -lt 180; $i++) {
            Start-Sleep -Seconds 1
            $l = Join-Path $app 'logs\moonshine_app.log'
            if ((Test-Path -LiteralPath $l) -and (Select-String -LiteralPath $l -Pattern 'following the log' -Quiet)) { $up = $true; break }
        }
        if ($up) { Write-Output "  moonshine_app.exe following $Game\kcd.log after ${i}s, under Moonshine.ps1 -NoGame" }
        else { Fault 'the launcher did not get the app to following the log within 180s'; if (Test-Path $appLog) { Get-Content $appLog -Tail 15 | ForEach-Object { "  " + $_ } } }
    }
}

Write-Output ''
if ($faults.Count -eq 0) { Write-Output "OK: Moonshine $Version downloaded, installed, checked and conversed on $env:COMPUTERNAME. Logs in $work"; exit 0 }
Write-Output ("FAULTS (" + $faults.Count + "):"); $faults | ForEach-Object { "  " + $_ }
exit 1
