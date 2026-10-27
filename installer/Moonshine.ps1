# Start the Moonshine app and Kingdom Come: Deliverance 2 together. The desktop shortcut
# the installer writes runs this; so does the Start Menu entry.
#
#   powershell -NoExit -ExecutionPolicy Bypass -File Moonshine.ps1          play
#   powershell -ExecutionPolicy Bypass -File Moonshine.ps1 -Check           say what is found, and stop
#   powershell -ExecutionPolicy Bypass -File Moonshine.ps1 -Stop            stop the app (the game is left alone)
#   powershell -ExecutionPolicy Bypass -File Moonshine.ps1 -NoGame          the app alone; the game is
#                                                                            started some other way
#
# What it does, in order: makes sure every file the app will open is there
# (moonshine_app --where), asks Steam to start the game, and then runs the app in this
# window and keeps it running until /quit is typed or the window is closed. The game is
# not touched when the app dies: a restarted app reads the game's log from the end and
# sweeps the half-written turns, so a line that was cut off has to be said again, once
# this window says it is following the log.
#
# Derived from the development laptop's supervise and play scripts, minus the
# scheduled tasks those need to reach a desktop from an ssh session; this one is started
# from the desktop. The app's working directory is this folder, which is where it looks
# for setting.txt, data\ and models\, and where it writes logs\.
#
# moonshine.cfg beside this script, optional, one `key = value` a line:
#   root = D:\SteamLibrary\steamapps\common\KingdomComeDeliverance2    the game, when it is
#                                                                        not under a Steam
#                                                                        folder the app knows
#   args = --no-mic                                                      anything else for the app
param(
    [switch]$Check,
    [switch]$Stop,
    # Do not ask Steam for the game: for a build of the game that is not Steam's (the GOG
    # one on the cloud test machines), or a game already up that this should only join.
    [switch]$NoGame
)
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
Set-Location $here
try { $Host.UI.RawUI.WindowTitle = 'Moonshine' } catch {}

$app = Join-Path $here 'moonshine_app.exe'
$logDir = Join-Path $here 'logs'
$appLog = Join-Path $logDir 'moonshine_app.log'
$supervisorLog = Join-Path $logDir 'moonshine.log'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Write-Line([string]$line) {
    $text = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $line
    Write-Output $text
    Add-Content -LiteralPath $supervisorLog -Value $text -ErrorAction SilentlyContinue
}

# --- the settings file ---------------------------------------------------------------------
$cfg = @{}
$cfgPath = Join-Path $here 'moonshine.cfg'
if (Test-Path -LiteralPath $cfgPath) {
    foreach ($raw in Get-Content -LiteralPath $cfgPath) {
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        $i = $line.IndexOf('=')
        if ($i -lt 1) { continue }
        $cfg[$line.Substring(0, $i).Trim().ToLower()] = $line.Substring($i + 1).Trim()
    }
}

# --- what the app is started with ----------------------------------------------------------
# The weights are found by shape, not by name, so a drop that ships a different model
# changes nothing here: the one .gguf under models\llm, the one folder under models\stt.
function Format-Arg([string]$text) {
    if ($text -notmatch '[\s"]') { return $text }
    return '"' + ($text -replace '"', '\"') + '"'
}
$common = @()
$gguf = @(Get-ChildItem -LiteralPath (Join-Path $here 'models\llm') -Filter '*.gguf' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($gguf.Count -gt 0) { $common += @('--gguf', (Format-Arg ('models\llm\' + $gguf[0].Name))) }
$stt = @(Get-ChildItem -LiteralPath (Join-Path $here 'models\stt') -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
if ($stt.Count -gt 0) { $common += @('--stt-model', (Format-Arg ('models\stt\' + $stt[0].Name))) }
if ($cfg['root']) { $common += @('--root', (Format-Arg $cfg['root'])) }
if ($cfg['args']) { $common += $cfg['args'] }
# The voice on the CPU: the game has the GPU to itself. A gpu already in the environment
# is somebody's experiment and is kept.
if (-not $env:MOONSHINE_TTS_BACKEND) { $env:MOONSHINE_TTS_BACKEND = 'cpu' }

# --- stop ----------------------------------------------------------------------------------
# A window running the loop below would start the app again two seconds after -Stop killed
# it; this file, written first, tells that loop the death was asked for.
$stopMarker = Join-Path $logDir 'stop'
function Stop-App {
    Set-Content -LiteralPath $stopMarker -Value (Get-Date -Format 'o') -ErrorAction SilentlyContinue
    $rows = @(Get-Process moonshine_app -ErrorAction SilentlyContinue)
    foreach ($row in $rows) {
        Write-Line ("stopping moonshine_app.exe, pid " + $row.Id)
        Stop-Process -Id $row.Id -Force -ErrorAction SilentlyContinue
    }
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process moonshine_app -ErrorAction SilentlyContinue)) { return $true }
        Start-Sleep -Milliseconds 200
    }
    # A process that will not die and holds almost no memory is a husk the kernel has not
    # finished tearing down; it answers nothing, so it is not an app running beside ours.
    $live = @(Get-Process moonshine_app -ErrorAction SilentlyContinue | Where-Object { $_.WorkingSet64 -gt 1MB })
    return ($live.Count -eq 0)
}
if ($Stop) {
    if (Stop-App) { Write-Output 'stopped' } else { Write-Output 'an app is still running and would not stop' }
    return
}

# --- the check -----------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $app)) {
    Write-Output "no app at $app -- the installation is incomplete; run the installer again"
    if (-not $Check) { Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}
Write-Output "Moonshine: checking the installation ..."
& $app --where @common
$ready = ($LASTEXITCODE -eq 0)
if (-not $ready) {
    Write-Output ''
    Write-Output 'Something the app needs is missing (the lines marked MISSING above).'
    Write-Output 'If the game is installed somewhere the app did not look, put its folder in moonshine.cfg:'
    Write-Output '  root = D:\SteamLibrary\steamapps\common\KingdomComeDeliverance2'
    Write-Output 'Otherwise run the installer again, and if it still says so, run Collect-Logs.ps1 and send the zip.'
    if (-not $Check) { Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}
if ($Check) { return }

# --- the game ------------------------------------------------------------------------------
# Through Steam, which is the only start the Steam build accepts, and which also brings
# the client up when it is not. Started before the app so the game's own load, which is
# the longer one, is under way while the model loads; the app follows the log from the
# moment it appears.
if (-not (Stop-App)) {
    Write-Line 'an app already running would not stop. Not starting another beside it.'
    Read-Host 'Press Enter to close' | Out-Null
    exit 1
}
if ($NoGame) {
    Write-Line 'not starting the game (-NoGame); the app follows its log when it appears'
} elseif (Get-Process KingdomCome -ErrorAction SilentlyContinue) {
    Write-Line 'the game is already running; joining it'
} else {
    Write-Line 'asking Steam to start Kingdom Come: Deliverance 2'
    Start-Process 'steam://rungameid/1771300'
}

# --- the app, kept running -----------------------------------------------------------------
Write-Line 'Moonshine app. Leave this window open while you play; /quit here stops it. Closing it stops it too.'
Remove-Item -LiteralPath $stopMarker -Force -ErrorAction SilentlyContinue
$argsLine = (@('--game', '--world', 'setting.txt', '--log', (Format-Arg $appLog)) + $common) -join ' '
$fast = 0
while ($true) {
    # The app truncates its --log on open, which would erase the run it is restarting
    # from; the dead run's log is kept beside it (five of them).
    if (Test-Path -LiteralPath $appLog) {
        $kept = "$appLog." + (Get-Date -Format 'yyyyMMdd-HHmmss')
        Move-Item -LiteralPath $appLog -Destination $kept -Force -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $logDir -Filter 'moonshine_app.log.*' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -Skip 5 |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $app
    $info.Arguments = $argsLine
    $info.WorkingDirectory = $here
    # Inherited handles, not redirects: this window is where /quit is typed.
    $info.UseShellExecute = $false
    $code = 1
    $started = Get-Date
    $proc = $null
    try {
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $info
        [void]$proc.Start()
        $proc.WaitForExit()
        if ($null -ne $proc.ExitCode) { $code = [int]$proc.ExitCode }
    } catch {
        Write-Line ("could not start the app: " + $_.Exception.Message)
        $code = 1
    } finally {
        if ($proc) { $proc.Dispose() }
    }
    # 0 is /quit; 0xC000013A is Ctrl+C or the window closed on it; the marker is -Stop
    # from another window. Deliberate, all three.
    if ($code -eq 0 -or $code -eq -1073741510) {
        Write-Line "app stopped (exit $code)."
        break
    }
    if (Test-Path -LiteralPath $stopMarker) {
        Remove-Item -LiteralPath $stopMarker -Force -ErrorAction SilentlyContinue
        Write-Line 'app stopped (Moonshine.ps1 -Stop).'
        break
    }
    $life = [int]((Get-Date) - $started).TotalSeconds
    # Twenty seconds is under a model load, so a process that lived longer had come up.
    if ($life -lt 20) { $fast += 1 } else { $fast = 0 }
    $hex = '0x{0:X8}' -f ([int64]$code -band 0xFFFFFFFF)
    if ($fast -ge 5) {
        Write-Line "the app exited ($hex) after ${life}s, $fast times within 20s of starting. Not starting it again; run Collect-Logs.ps1 and send the zip."
        Read-Host 'Press Enter to close' | Out-Null
        break
    }
    $wait = 2
    if ($fast -gt 1) { $wait = [Math]::Min(16, [int][Math]::Pow(2, $fast)) }
    Write-Line "the app exited ($hex) after ${life}s. Starting it again in ${wait}s. Wait until it says it is following the log, then say again anything that was cut off."
    Start-Sleep -Seconds $wait
}
