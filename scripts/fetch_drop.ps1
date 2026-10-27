# The pinned data drop (data\DROP), into drop\ and assembled into data\kcd2\. All of the
# work is tools\fetch_drop.py; this finds a Python for it. -Python names one; otherwise the
# first python.exe on the PATH (not the Microsoft Store stub).
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_drop.ps1 -From D:\drops
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_drop.ps1 -From s3://bucket/drops   (S3_ENDPOINT, AWS_* set)
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_drop.ps1 -Check
param(
    [string]$From = $env:MOONSHINE_DROP_BASE,
    [string]$Python = $env:PYTHON,
    [switch]$Check,
    [switch]$Assemble,
    [switch]$NoAssemble
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
if (-not $Python) {
    $cmd = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1
    if (-not $cmd) { throw 'no python on the PATH; -Python C:\path\to\python.exe' }
    $Python = $cmd.Source
}
$argv = @('tools\fetch_drop.py')
if ($From) { $argv += @('--from', $From) }
if ($Check) { $argv += '--check' }
if ($Assemble) { $argv += '--assemble' }
if ($NoAssemble) { $argv += '--no-assemble' }
& $Python @argv
exit $LASTEXITCODE
