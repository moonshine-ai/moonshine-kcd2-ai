# The Phonon voice model made from source, into models\phonon: the Windows half of
# scripts/fetch_phonon.sh, which says why you normally want scripts\fetch_models.ps1 -Only
# phonon (a download, pinned) instead. Needs python (3.10 to 3.12),
# git, about 3 GB of disk for torch, and a few minutes.
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_phonon.ps1 [-Force] [-Check]
param(
    [switch]$Force,
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$srcUrl = if ($env:MOONSHINE_SRC_URL) { $env:MOONSHINE_SRC_URL } else { 'https://github.com/moonshine-ai/moonshine.git' }
$srcRev = if ($env:MOONSHINE_SRC_REV) { $env:MOONSHINE_SRC_REV } else { 'd8d8fa29d05ca67441a7719e70099f9f48917a93' }
$out = 'models\phonon'

function Test-Complete([string]$dir) {
    foreach ($f in 'backbone.ort', 'embed_latent.ort', 'embed_prompt.ort', 'flow_head.ort',
                   'mimi_seanet.ort', 'mimi_transformer.ort', 'model.json', 'tokenizer.bin',
                   'voices.json') {
        $p = Join-Path $dir $f
        if (-not (Test-Path -LiteralPath $p) -or (Get-Item -LiteralPath $p).Length -eq 0) { return $false }
    }
    return [bool](Get-ChildItem -LiteralPath (Join-Path $dir 'voices') -Filter *.safetensors -ErrorAction SilentlyContinue)
}
function Run([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed ($LASTEXITCODE)" }
}

if ((Test-Complete $out) -and -not $Force) {
    $n = (Get-ChildItem (Join-Path $out 'voices') -Filter *.safetensors).Count
    Write-Output "  ok       $out ($n voices)"
    exit 0
}
if ($Check) { Write-Error "  missing  $out; run scripts\fetch_phonon.ps1"; exit 1 }

$work = 'build\phonon-export'
New-Item -ItemType Directory -Force $work | Out-Null

if (-not (Test-Path "$work\src\scripts\export-phonon-model.py")) {
    if (Test-Path "$work\src") { Remove-Item -Recurse -Force "$work\src" }
    Run git @('clone', '--quiet', '--filter=blob:none', '--sparse', $srcUrl, "$work\src")
    Run git @('-C', "$work\src", 'sparse-checkout', 'set', 'scripts')
    Run git @('-C', "$work\src", 'checkout', '--quiet', $srcRev)
}

$py = "$work\venv\Scripts\python.exe"
if (-not (Test-Path $py)) {
    Run python @('-m', 'venv', "$work\venv")
    Run $py @('-m', 'pip', 'install', '--quiet', '--upgrade', 'pip')
    Run $py @('-m', 'pip', 'install', '--quiet', 'torch', 'transformers>=5.21', 'onnx',
              'onnxruntime', 'safetensors', 'numpy', 'huggingface_hub')
}

if (Test-Path "$out.part") { Remove-Item -Recurse -Force "$out.part" }
Run $py @("$work\src\scripts\export-phonon-model.py", '--out-dir', "$out.part")
Remove-Item -Force "$out.part\reference.f32" -ErrorAction SilentlyContinue
if (-not (Test-Complete "$out.part")) { throw "the export finished but $out.part is incomplete" }
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
Move-Item "$out.part" $out
Write-Output "phonon is in place under $PWD\$out"
