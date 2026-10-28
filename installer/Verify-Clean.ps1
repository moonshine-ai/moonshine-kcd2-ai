# Run the installer as somebody who has never had Moonshine: a fresh local account on this
# machine, whose profile has no Ollama, no moonshine_voice cache, no checkout and no
# Saved Games\kingdomcome2 -- only what the installer brings, and the game that Steam
# installed for everybody. Then the launcher's own check from that account, and a real
# conversation (model, embedder, voice) against the installed tree with the fake game.
#
#   powershell -ExecutionPolicy Bypass -File installer\Verify-Clean.ps1 -Version 0.9.94 [-Keep] [-Download]
#
# Needs an administrator (the account is made and removed here). The
# installer and the checks run as the test account through a scheduled task, which is the
# one way to run a program as another user from an ssh session without a desktop. The
# account is removed at the end unless -Keep, profile and all; the installer's log, the
# check's output and the conversation's transcript are left under <Repo>\release\verify\.
#
# The installer downloads the models and data; here, on the machine that has the stage,
# it is given /MIRROR=<Repo>\release\stage\Moonshine and copies them from there instead,
# so the test is of the installer and the app, not of the hosting. -Download leaves the
# mirror out, for a test of the published URLs too (8 GB, and the files must be up).
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Repo = '',
    [string]$Python = $env:PYTHON,
    [string]$User = 'moontest',
    [switch]$Keep,
    [switch]$Download
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has $PSScriptRoot empty while the parameter defaults are evaluated.
if (-not $Repo) { $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
if (-not $Python) {
    $cmd = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1
    if (-not $cmd) { Write-Error 'no python on the PATH; -Python C:\path\to\python.exe'; exit 1 }
    $Python = $cmd.Source
}
$out = Join-Path $Repo "release\out\$Version"
$setup = Join-Path $out "Moonshine-$Version-setup.exe"
if (-not (Test-Path -LiteralPath $setup)) { Write-Error "no installer at $setup"; exit 1 }
$report = Join-Path $Repo 'release\verify'
New-Item -ItemType Directory -Force -Path $report | Out-Null
# Somewhere every account can read and write: the installer's copy, the task's outputs.
$share = 'C:\Users\Public\MoonshineVerify'
New-Item -ItemType Directory -Force -Path $share | Out-Null
$faults = @()
function Step([string]$t) { Write-Output ("=== " + $t) }
function Fault([string]$t) { $script:faults += $t; Write-Output ("  FAULT: " + $t) }

# --- the account ---------------------------------------------------------------------------
# A throwaway password; the account dies with this script. In Administrators only so a
# scheduled task may run as it (plain Users lack "log on as a batch job"); the installer
# never asks for elevation, so that changes nothing about what it does. The name carries
# the time: Windows keeps a profile's registry hive loaded for a while after the last
# process in it ends, and a second account of the same name would then be given
# C:\Users\<name>.<machine> instead, which is a surprise worth not having. Older test
# profiles that can be removed are removed first.
Step 'account'
foreach ($old in Get-CimInstance Win32_UserProfile | Where-Object { $_.LocalPath -like 'C:\Users\moontest*' }) {
    try { Remove-CimInstance $old -ErrorAction Stop; Write-Output ("  removed old profile " + $old.LocalPath) }
    catch { Write-Output ("  old profile " + $old.LocalPath + " is still held (hive loaded); it goes after a reboot") }
}
foreach ($old in Get-LocalUser | Where-Object { $_.Name -like 'moontest*' }) { Remove-LocalUser -Name $old.Name -ErrorAction SilentlyContinue }
if ($User -eq 'moontest') { $User = 'moontest' + (Get-Date -Format 'HHmm') }
Write-Output "  $User"
$password = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 20 | ForEach-Object { [char]$_ })
$password = 'Mv!' + $password
$secure = ConvertTo-SecureString $password -AsPlainText -Force
New-LocalUser -Name $User -Password $secure -PasswordNeverExpires -AccountNeverExpires -Description 'Moonshine installer test' | Out-Null
Add-LocalGroupMember -Group 'Administrators' -Member $User
$sid = (Get-LocalUser -Name $User).SID.Value

# Where Windows put the account's profile, once a task has run as it and created one.
function Profile-Dir {
    $p = Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $sid } | Select-Object -First 1
    if ($p) { return $p.LocalPath }
    return "C:\Users\$User"
}

# Run a command line as the test account and wait for it. The line goes into a .cmd file
# (quotes do not survive schtasks /TR from PowerShell); the task's exit code is read back,
# and stdout goes where the line sends it.
function Run-As([string]$name, [string]$commandLine, [int]$timeoutSeconds) {
    $task = "MoonshineVerify-$name"
    $cmdFile = Join-Path $share "$name.cmd"
    Set-Content -LiteralPath $cmdFile -Value ("@echo off`r`n" + $commandLine + "`r`nexit /b %ERRORLEVEL%`r`n") -Encoding ASCII
    # schtasks talks on stderr for a task that is not there, which under Stop is a throw.
    $ErrorActionPreference = 'Continue'
    & schtasks /Query /TN $task 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { & schtasks /Delete /TN $task /F 2>&1 | Out-Null }
    $created = & schtasks /Create /TN $task /TR "$cmdFile" /SC ONCE /ST 00:00 /RU $User /RP $password /RL LIMITED /F 2>&1
    if ($LASTEXITCODE -ne 0) { $ErrorActionPreference = 'Stop'; throw "could not create the task ${task}: $created" }
    & schtasks /Run /TN $task 2>&1 | Out-Null
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    Start-Sleep -Seconds 3
    while ((Get-Date) -lt $deadline) {
        $status = (& schtasks /Query /TN $task /FO LIST /V | Select-String '^Status:').Line
        if ($status -notmatch 'Running') { break }
        Start-Sleep -Seconds 5
    }
    $result = (& schtasks /Query /TN $task /FO LIST /V | Select-String '^Last Result:').Line -replace '^Last Result:\s*', ''
    & schtasks /Delete /TN $task /F 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    $code = [int64]$result
    if ($code -gt 2147483647) { $code -= 4294967296 }
    return [int]$code
}

try {
    # --- the install -----------------------------------------------------------------------
    # The installer is copied where the test account can read it.
    Step 'install'
    $setupCopy = Join-Path $share 'setup'
    if (Test-Path -LiteralPath $setupCopy) { Remove-Item -LiteralPath $setupCopy -Recurse -Force }
    & robocopy $out $setupCopy /E /NFL /NDL /NJH /NJS /NP | Out-Null
    $installLog = Join-Path $share 'install.log'
    if (Test-Path -LiteralPath $installLog) { Remove-Item -LiteralPath $installLog -Force }
    # The mod task is left out: this machine has the Workshop mod, and the test is of the app.
    $cmd = "`"$setupCopy\Moonshine-$Version-setup.exe`" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /MERGETASKS=!copymod /LOG=`"$installLog`""
    if (-not $Download) { $cmd += " /MIRROR=`"$(Join-Path $Repo 'release\stage\Moonshine')`"" }
    $code = Run-As 'install' $cmd $(if ($Download) { 7200 } else { 1800 })
    Write-Output "  installer exit $code"
    if ($code -ne 0) { Fault "the installer exited $code" }
    # The [Code] logs a line per file each time Setup asks its Check, so distinct names.
    $fromWeb = @(Select-String -LiteralPath $installLog -Pattern 'to download: ' -ErrorAction SilentlyContinue | ForEach-Object { $_.Line -replace '^.*to download: ' } | Sort-Object -Unique).Count
    $fromMirror = @(Select-String -LiteralPath $installLog -Pattern 'from the mirror: ' -ErrorAction SilentlyContinue | ForEach-Object { $_.Line -replace '^.*from the mirror: ' } | Sort-Object -Unique).Count
    Write-Output "  files: $fromWeb downloaded, $fromMirror from the mirror"
    Copy-Item -LiteralPath $installLog -Destination (Join-Path $report 'install.log') -Force -ErrorAction SilentlyContinue
    $home_ = Profile-Dir
    Write-Output "  profile: $home_"
    $app = Join-Path $home_ 'AppData\Local\Moonshine'
    if (-not (Test-Path -LiteralPath (Join-Path $app 'moonshine_app.exe'))) { Fault "no moonshine_app.exe under $app" }
    foreach ($need in @('Moonshine.ps1', 'Fetch-Package.ps1', 'Collect-Logs.ps1', 'README.txt', 'VERSION.txt', 'setting.txt', 'manifest.json', 'LICENSES\README.txt',
                        'data\kcd2\npcs.sqlite', 'models\embeddinggemma-300m-q8_0.gguf', 'models\qwen3-tts-0.6b\model.safetensors',
                        'libopenblas.dll', 'moonshine.dll', 'onnxruntime.dll', 'moonshine.cfg')) {
        if (-not (Test-Path -LiteralPath (Join-Path $app $need))) { Fault "installed tree lacks $need" }
    }
    if (Test-Path -LiteralPath (Join-Path $app 'moonshine.cfg')) { Write-Output ("  moonshine.cfg: " + ((Get-Content -LiteralPath (Join-Path $app 'moonshine.cfg')) -join ' | ')) }
    $llm = @(Get-ChildItem -LiteralPath (Join-Path $app 'models\llm') -Filter '*.gguf' -ErrorAction SilentlyContinue)
    if ($llm.Count -ne 1) { Fault "expected one LLM gguf, found $($llm.Count)" }
    $stt = @(Get-ChildItem -LiteralPath (Join-Path $app 'models\stt') -Directory -ErrorAction SilentlyContinue)
    if ($stt.Count -ne 1) { Fault "expected one STT folder, found $($stt.Count)" }
    foreach ($link in @("$home_\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Moonshine\Moonshine.lnk",
                        "$home_\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Moonshine\Collect Moonshine logs.lnk")) {
        if (-not (Test-Path -LiteralPath $link)) { Fault "no shortcut $link" }
    }
    if (Test-Path -LiteralPath (Join-Path $app 'unins000.exe')) { Write-Output '  uninstaller present' } else { Fault 'no uninstaller' }
    $installed = (Get-ChildItem -LiteralPath $app -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Output ("  installed: " + [math]::Round($installed / 1GB, 2) + " GB")

    # --- the launcher's check, as that account ---------------------------------------------
    Step 'check'
    $checkLog = Join-Path $share 'check.log'
    if (Test-Path -LiteralPath $checkLog) { Remove-Item -LiteralPath $checkLog -Force }
    $cmd = "cmd /c powershell -NoProfile -ExecutionPolicy Bypass -File `"$app\Moonshine.ps1`" -Check > `"$checkLog`" 2>&1"
    $code = Run-As 'check' $cmd 300
    Get-Content -LiteralPath $checkLog -ErrorAction SilentlyContinue | ForEach-Object { "  " + $_ }
    Copy-Item -LiteralPath $checkLog -Destination (Join-Path $report 'check.log') -Force -ErrorAction SilentlyContinue
    if ($code -ne 0) { Fault "Moonshine.ps1 -Check exited $code" }
    $checkText = (Get-Content -LiteralPath $checkLog -Raw -ErrorAction SilentlyContinue)
    if ($checkText -notmatch '(?m)^ready') { Fault 'the check did not end in ready' }
    if ($checkText -match 'MISSING') { Fault 'the check reports something MISSING' }

    # --- a conversation, from the installed tree -------------------------------------------
    # The fake game (scripts/fake_game.py) stands in for the real one: it writes the log
    # lines the mod would, and reads the turns the app answers with. Run as the
    # administrator this script runs as (the python is theirs; the tree is the test
    # account's, which an administrator may read), from the installed folder, so every
    # relative path is the one a tester has. Not canned: the shipped weights answer.
    Step 'conversation'
    $convLog = Join-Path $report 'conversation.log'
    Push-Location $app
    try {
        $env:MOONSHINE_TTS_BACKEND = 'cpu'
        & $Python (Join-Path $Repo 'scripts\fake_game.py') --app (Join-Path $app 'moonshine_app.exe') --world setting.txt `
            --gguf ('models\llm\' + $llm[0].Name) --personas 'data/kcd2/npcs.sqlite' --warmup 240 --timeout 120 2>&1 |
            Tee-Object -FilePath $convLog | ForEach-Object { "  " + $_ }
        if ($LASTEXITCODE -ne 0) { Fault "fake_game.py exited $LASTEXITCODE" }
    } finally { Pop-Location }

    # --- the uninstaller -------------------------------------------------------------------
    Step 'uninstall'
    $cmd = "`"$app\unins000.exe`" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
    $code = Run-As 'uninstall' $cmd 600
    Start-Sleep -Seconds 3
    if (Test-Path -LiteralPath (Join-Path $app 'moonshine_app.exe')) { Fault 'the uninstaller left moonshine_app.exe' }
    if (Test-Path -LiteralPath (Join-Path $app 'models')) { Fault 'the uninstaller left models\' }
    Write-Output "  uninstaller exit $code"
} finally {
    if (-not $Keep) {
        Step "removing $User"
        # Anything still running as the account holds its profile open.
        Get-Process -IncludeUserName -ErrorAction SilentlyContinue | Where-Object { $_.UserName -like "*\$User" } |
            Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Remove-LocalUser -Name $User -ErrorAction SilentlyContinue
        # The profile folder: Remove-LocalUser leaves it. Win32_UserProfile deletes it with its
        # registry entry, when the hive is not still loaded by the task scheduler's session.
        $p = Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $sid }
        if ($p) {
            try { Remove-CimInstance $p -ErrorAction Stop; Write-Output ("  removed " + $p.LocalPath) }
            catch { Write-Output ("  " + $p.LocalPath + " is still held (its hive is loaded); the next run, or a reboot, removes it") }
        }
        Remove-Item -LiteralPath $share -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Output "kept: account $User, password $password, $share"
    }
}

Write-Output ''
if ($faults.Count -eq 0) {
    Write-Output "OK: Moonshine $Version installs, checks and converses from a clean account. Reports in $report"
    exit 0
}
Write-Output ("FAULTS (" + $faults.Count + "):")
$faults | ForEach-Object { "  " + $_ }
exit 1
