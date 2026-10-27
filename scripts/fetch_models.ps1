# The model weights, into models\: every file in scripts\models.tsv, each checked against
# its pinned size and sha256, and skipped when it is already there and right. The Windows
# half of scripts/fetch_models.sh; the table is the same.
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_models.ps1 [-Only stt] [-Check]
param(
    [string]$Only = '',
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
# Invoke-WebRequest's progress bar makes big downloads many times slower; off.
$ProgressPreference = 'SilentlyContinue'

function Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

if ((-not $Only -or 'embeddinggemma'.StartsWith($Only)) -and -not $Check) {
    Write-Output 'embeddinggemma-300m is distributed under the Gemma Terms of Use'
    Write-Output '(https://ai.google.dev/gemma/terms); fetching it means accepting them.'
}

$missing = 0
$wrong = 0
foreach ($line in Get-Content -LiteralPath 'scripts\models.tsv') {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $dest, $url, $bytes, $want = $line.Split("`t")
    if ($Only -and -not $dest.StartsWith($Only)) { continue }
    $path = Join-Path 'models' ($dest -replace '/', '\')
    if (Test-Path -LiteralPath $path) {
        $have = (Get-Item -LiteralPath $path).Length
        if ($have -eq [long]$bytes -and (Sha256 $path) -eq $want) {
            Write-Output "  ok       $dest"
            continue
        }
        Write-Output "  wrong    $dest ($have bytes, want $bytes, or the sha256 differs)"
    } else {
        Write-Output "  missing  $dest"
    }
    if ($Check) { $missing++; continue }
    Write-Output ("=== fetching $dest (" + [math]::Round([long]$bytes / 1MB) + " MB) ===")
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    $part = "$path.part"
    # curl.exe ships with Windows 10 1803 and later and resumes; Invoke-WebRequest is the
    # fallback for a machine without it.
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        & curl.exe -fL --retry 3 -s -S -o $part $url
        if ($LASTEXITCODE -ne 0) { Write-Error "download of $dest failed"; $wrong++; continue }
    } else {
        Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
    }
    $got = Sha256 $part
    if ($got -ne $want) {
        Write-Error "$dest`: downloaded sha256 $got, want $want; left as $part"
        $wrong++
        continue
    }
    Move-Item -LiteralPath $part -Destination $path -Force
}

if ($Check -and $missing -gt 0) { Write-Error "$missing file(s) missing or wrong; run without -Check"; exit 1 }
if ($wrong -gt 0) { exit 1 }
Write-Output ("models are in place under " + (Join-Path (Get-Location).Path 'models'))
