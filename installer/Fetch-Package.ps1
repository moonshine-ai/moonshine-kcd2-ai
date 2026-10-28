# Bring the big files of a Moonshine installation into place from the manifest that lists
# them: the models, the database and the mod, around 8 GB. The installer downloads them
# itself the first time (Inno Setup's own downloader, from the same list); this is for
# afterwards: the Start Menu's "Download or repair Moonshine files" runs it against the
# installed manifest, Moonshine.ps1 -Update runs it against the newest one, and the in-app
# Update of the future is this script's logic over the same manifest, in the app.
#
#   powershell -ExecutionPolicy Bypass -File Fetch-Package.ps1 [-Manifest <file or https URL>]
#       [-Dest <folder>] [-ModTo <game>\mods] [-Mirror <folder>] [-Check]
#
# The manifest is JSON:
#
#   { "version": "0.9.123",
#     "latest": "https://.../latest/manifest.json",            where -Update looks
#     "setup": { "url": "https://.../Moonshine-0.9.123-setup.exe", "bytes": ..., "sha256": "..." },
#     "files": [ { "path": "models/llm/Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
#                  "bytes": 2497281120,
#                  "sha256": "3605...597",
#                  "url": "https://huggingface.co/.../Qwen3-4B-Instruct-2507-Q4_K_M.gguf" },
#                ... ] }
#
# `path` is relative to -Dest (this script's folder unless told). A file already there with
# the right size and sha256 is left alone, so a second run, a repair and an update all move
# only what differs. Downloads go to <path>.part and resume from where they stopped; a
# transfer that stalls for a minute is retried, five times with a pause between, and the
# bytes are checked against the manifest before the file is put in place. -Mirror names a
# folder with the same layout (a staged package on the machine that built it): a file found
# there with the right hash is copied rather than downloaded. -ModTo names the game's mods
# folder; after the files are in, mod\moonshine_names is mirrored to <ModTo>\moonshine_names.
# -Check reports and changes nothing; exit code 1 when anything is missing or wrong.
#
# Progress goes to the console, one line per file; everything also goes to logs\fetch.log
# under -Dest, which Collect-Logs.ps1 picks up.
param(
    [string]$Manifest = '',
    [string]$Dest = '',
    [string]$ModTo = '',
    [string]$Mirror = '',
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
if (-not $Dest) { $Dest = $PSScriptRoot }
if (-not $Manifest) { $Manifest = Join-Path $Dest 'manifest.json' }
$logDir = Join-Path $Dest 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logFile = Join-Path $logDir 'fetch.log'

function Log([string]$line) {
    $text = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $line
    Write-Host $line
    Add-Content -LiteralPath $logFile -Value $text -ErrorAction SilentlyContinue
}
function Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function MB([long]$n) { [math]::Round($n / 1MB) }

# --- HTTP ----------------------------------------------------------------------------------
# One HttpClient for the run. No overall timeout: a 2.5 GB file on a slow line takes what
# it takes; a transfer that stops moving is what is caught, below.
Add-Type -AssemblyName System.Net.Http
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
$handler = New-Object System.Net.Http.HttpClientHandler
$handler.AllowAutoRedirect = $true
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromMilliseconds(-1)
$client.DefaultRequestHeaders.UserAgent.ParseAdd('Moonshine-Fetch/1.0')

function Get-Text([string]$url) {
    $resp = $client.GetAsync($url).GetAwaiter().GetResult()
    if (-not $resp.IsSuccessStatusCode) { throw "HTTP $([int]$resp.StatusCode) for $url" }
    return $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
}

# One attempt at a download, resuming <part> if it is there. Returns when the server has
# sent everything it has; the caller checks the size and the hash.
function Download-Once([string]$url, [string]$part, [long]$expected, [string]$label) {
    $have = 0L
    if (Test-Path -LiteralPath $part) { $have = (Get-Item -LiteralPath $part).Length }
    if ($have -ge $expected -and $expected -gt 0) { return }
    $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, $url)
    if ($have -gt 0) { $req.Headers.Range = New-Object System.Net.Http.Headers.RangeHeaderValue($have, $null) }
    $resp = $client.SendAsync($req, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
    try {
        $code = [int]$resp.StatusCode
        $append = $false
        if ($code -eq 206) {
            $append = $true
        } elseif ($code -eq 200) {
            # The server ignored the range: start over.
            if ($have -gt 0) { Remove-Item -LiteralPath $part -Force; $have = 0 }
        } elseif ($code -eq 416 -and $have -gt 0) {
            # Nothing past what is here; let the size check decide.
            return
        } else {
            throw "HTTP $code"
        }
        $in = $resp.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $mode = if ($append) { [IO.FileMode]::Append } else { [IO.FileMode]::Create }
        $out = New-Object IO.FileStream($part, $mode, [IO.FileAccess]::Write, [IO.FileShare]::None, 1MB)
        try {
            $buf = New-Object byte[] (1MB)
            $done = $have
            $t0 = [Diagnostics.Stopwatch]::StartNew()
            $lastShown = -1.0
            $windowStart = $t0.Elapsed.TotalSeconds; $windowBytes = 0L; $rate = 0.0
            while ($true) {
                $task = $in.ReadAsync($buf, 0, $buf.Length)
                if (-not $task.Wait(60000)) { throw 'the transfer stalled for 60 s' }
                $n = $task.Result
                if ($n -le 0) { break }
                $out.Write($buf, 0, $n)
                $done += $n; $windowBytes += $n
                $now = $t0.Elapsed.TotalSeconds
                if ($now - $windowStart -ge 2) { $rate = $windowBytes / ($now - $windowStart) / 1MB; $windowStart = $now; $windowBytes = 0 }
                if ($now - $lastShown -ge 0.5) {
                    $pct = if ($expected -gt 0) { [math]::Floor($done * 100 / $expected) } else { 0 }
                    Write-Host ("`r  {0,3}%  {1,6} / {2,-6} MB  {3,5:N1} MB/s   {4}" -f $pct, (MB $done), (MB $expected), $rate, $label) -NoNewline
                    $lastShown = $now
                }
            }
            Write-Host ("`r" + (' ' * 78) + "`r") -NoNewline
        } finally { $out.Dispose(); $in.Dispose() }
    } finally { $resp.Dispose() }
}

# The whole of one file: attempts, size check, hash check.
function Fetch-File($entry, [string]$target) {
    $part = "$target.part"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    $attempt = 0
    while ($true) {
        $attempt++
        try {
            Download-Once $entry.url $part ([long]$entry.bytes) $entry.path
            $size = (Get-Item -LiteralPath $part).Length
            if ($size -ne [long]$entry.bytes) { throw "got $size bytes, the manifest says $($entry.bytes)" }
            $got = Sha256 $part
            if ($got -ne $entry.sha256) { throw "sha256 $got, the manifest says $($entry.sha256)" }
            Move-Item -LiteralPath $part -Destination $target -Force
            return
        } catch {
            $why = $_.Exception.Message
            if ($_.Exception.InnerException) { $why += ' (' + $_.Exception.InnerException.Message + ')' }
            # A wrong size or hash means the bytes there are bad, not short; start again.
            if ($why -match 'sha256|bytes, the manifest') { Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue }
            if ($attempt -ge 5) { throw "$($entry.path): $why, after $attempt attempts" }
            $wait = [math]::Min(60, 5 * $attempt)
            Log ("  retry {0}/5 for {1} in {2}s: {3}" -f $attempt, $entry.path, $wait, $why)
            Start-Sleep -Seconds $wait
        }
    }
}

# --- the manifest --------------------------------------------------------------------------
if ($Manifest -match '^https?://') {
    Log "manifest: $Manifest"
    $json = Get-Text $Manifest
} else {
    if (-not (Test-Path -LiteralPath $Manifest)) { Log "no manifest at $Manifest"; exit 1 }
    $json = Get-Content -LiteralPath $Manifest -Raw
}
$m = $json | ConvertFrom-Json
if (-not $m.files) { Log 'the manifest lists no files'; exit 1 }
Log ("Moonshine {0}: {1} files, {2} MB, into {3}" -f $m.version, $m.files.Count, (MB (($m.files | Measure-Object -Property bytes -Sum).Sum)), $Dest)

# --- the files -----------------------------------------------------------------------------
$ok = 0; $fetched = 0; $copied = 0; $bad = @()
foreach ($f in $m.files) {
    $target = Join-Path $Dest ($f.path -replace '/', '\')
    $state = 'missing'
    if (Test-Path -LiteralPath $target) {
        $size = (Get-Item -LiteralPath $target).Length
        if ($size -eq [long]$f.bytes -and (Sha256 $target) -eq $f.sha256) { $state = 'ok' } else { $state = 'wrong' }
    }
    if ($state -eq 'ok') { $ok++; Log ("  ok        {0}" -f $f.path); continue }
    if ($Check) { $bad += $f.path; Log ("  {0,-9} {1}" -f $state, $f.path); continue }
    if ($Mirror) {
        $src = Join-Path $Mirror ($f.path -replace '/', '\')
        if ((Test-Path -LiteralPath $src) -and (Get-Item -LiteralPath $src).Length -eq [long]$f.bytes -and (Sha256 $src) -eq $f.sha256) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -LiteralPath $src -Destination $target -Force
            $copied++; Log ("  copied    {0}  (from the mirror)" -f $f.path); continue
        }
    }
    Log ("  fetching  {0}  ({1} MB)" -f $f.path, (MB ([long]$f.bytes)))
    try {
        Fetch-File $f $target
        $fetched++
    } catch {
        $bad += $f.path
        Log ("  FAILED    " + $_.Exception.Message)
    }
}

if ($Check) {
    if ($bad.Count -gt 0) { Log ("{0} file(s) missing or wrong" -f $bad.Count); exit 1 }
    Log 'all files present and correct'; exit 0
}
if ($bad.Count -gt 0) {
    Log ("{0} of {1} files could not be fetched: {2}" -f $bad.Count, $m.files.Count, ($bad -join ', '))
    Log 'Run this again to resume; partial downloads are kept.'
    exit 1
}
Log ("done: {0} already right, {1} downloaded, {2} copied" -f $ok, $fetched, $copied)

# --- the mod, into the game ----------------------------------------------------------------
if ($ModTo) {
    $modSrc = Join-Path $Dest 'mod\moonshine_names'
    if (-not (Test-Path -LiteralPath (Join-Path $modSrc 'mod.manifest'))) {
        Log "no mod at $modSrc; nothing copied to the game"
        exit 1
    }
    $modDst = Join-Path $ModTo 'moonshine_names'
    New-Item -ItemType Directory -Force -Path $ModTo | Out-Null
    & robocopy $modSrc $modDst /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:2 | Out-Null
    if ($LASTEXITCODE -ge 8) { Log "copying the mod to $modDst failed (robocopy $LASTEXITCODE)"; exit 1 }
    $global:LASTEXITCODE = 0
    Log "mod copied to $modDst"
}
exit 0
