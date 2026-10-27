# Gather what is needed to look into a problem, into one zip on the desktop.
#
#   powershell -ExecutionPolicy Bypass -File Collect-Logs.ps1
#
# What goes in: the app's logs (logs\), what moonshine_app --where says it finds, the
# game's own kcd.log (where the BRIDGE lines the app answers are), the machine in a few
# lines (Windows, CPU, memory, GPU), and the installer's log if it is still in TEMP.
# Nothing else: no saves, no conversation turns beyond what the logs already quote.
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
Set-Location $here

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $env:TEMP "moonshine-logs-$stamp"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# The app's own account of the installation, and where it thinks the game is.
$app = Join-Path $here 'moonshine_app.exe'
$where = Join-Path $work 'where.txt'
if (Test-Path -LiteralPath $app) {
    $common = @()
    $gguf = @(Get-ChildItem -LiteralPath (Join-Path $here 'models\llm') -Filter '*.gguf' -ErrorAction SilentlyContinue | Sort-Object Name)
    if ($gguf.Count -gt 0) { $common += @('--gguf', ('models\llm\' + $gguf[0].Name)) }
    $stt = @(Get-ChildItem -LiteralPath (Join-Path $here 'models\stt') -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
    if ($stt.Count -gt 0) { $common += @('--stt-model', ('models\stt\' + $stt[0].Name)) }
    $cfgPath = Join-Path $here 'moonshine.cfg'
    if (Test-Path -LiteralPath $cfgPath) {
        Copy-Item -LiteralPath $cfgPath -Destination $work -ErrorAction SilentlyContinue
        foreach ($raw in Get-Content -LiteralPath $cfgPath) {
            if ($raw -match '^\s*root\s*=\s*(.+)$') { $common += @('--root', $Matches[1].Trim()) }
        }
    }
    & $app --where @common 2>&1 | Out-File -LiteralPath $where -Encoding utf8
} else {
    "no moonshine_app.exe in $here" | Out-File -LiteralPath $where -Encoding utf8
}

# The app's logs: the live one and the kept runs.
$logDir = Join-Path $here 'logs'
if (Test-Path -LiteralPath $logDir) {
    Copy-Item -Path (Join-Path $logDir '*') -Destination $work -Recurse -ErrorAction SilentlyContinue
}

# The game's log, from the root --where reported.
$root = $null
foreach ($line in Get-Content -LiteralPath $where -ErrorAction SilentlyContinue) {
    if ($line -match '^game root:\s*(.+?)\s*$' -and $Matches[1] -notmatch 'MISSING') { $root = $Matches[1] }
}
if ($root -and (Test-Path -LiteralPath (Join-Path $root 'kcd.log'))) {
    Copy-Item -LiteralPath (Join-Path $root 'kcd.log') -Destination $work -ErrorAction SilentlyContinue
    # Which mods the game saw, in one line each, so the Workshop question is answered without
    # reading the whole log.
    Select-String -LiteralPath (Join-Path $root 'kcd.log') -Pattern '\[Mod\]|Mods used while saving|^\* mod ' |
        ForEach-Object { $_.Line } | Out-File -LiteralPath (Join-Path $work 'mods-seen.txt') -Encoding utf8
    $order = Join-Path $root 'mods\mod_order.txt'
    if (Test-Path -LiteralPath $order) { Copy-Item -LiteralPath $order -Destination (Join-Path $work 'mod_order.txt') }
}

# The machine.
$sys = Join-Path $work 'system.txt'
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $gpus = Get-CimInstance Win32_VideoController
    @(
        "windows: $($os.Caption) $($os.Version)"
        "cpu:     $($cpu.Name) ($($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) threads)"
        "memory:  $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB, $([math]::Round($os.FreePhysicalMemory / 1MB, 1)) GB free"
        ($gpus | ForEach-Object { "gpu:     $($_.Name), driver $($_.DriverVersion)" })
        "install: $here"
        "version: $(if (Test-Path -LiteralPath (Join-Path $here 'VERSION.txt')) { Get-Content -LiteralPath (Join-Path $here 'VERSION.txt') } else { 'unknown' })"
    ) | Out-File -LiteralPath $sys -Encoding utf8
} catch {
    "could not read the machine: $($_.Exception.Message)" | Out-File -LiteralPath $sys -Encoding utf8
}

# The installer's log, if Windows still has it.
Get-ChildItem -Path $env:TEMP -Filter 'Setup Log*.txt' -ErrorAction SilentlyContinue |
    Where-Object { (Get-Content -LiteralPath $_.FullName -TotalCount 40 -ErrorAction SilentlyContinue) -match 'Moonshine' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 |
    Copy-Item -Destination (Join-Path $work 'installer.log') -ErrorAction SilentlyContinue

$desktop = [Environment]::GetFolderPath('Desktop')
if (-not $desktop) { $desktop = $here }
$zip = Join-Path $desktop "Moonshine-logs-$stamp.zip"
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
Write-Output "written: $zip"
Write-Output 'Send that file along with a sentence or two about what you were doing.'
if (-not $env:MOONSHINE_NO_PAUSE) { Read-Host 'Press Enter to close' | Out-Null }
