#!/usr/bin/env bash
# The model weights, into models/: every file in scripts/models.tsv, each checked against
# its pinned size and sha256, and skipped when it is already there and right. macOS and
# Linux; scripts/fetch_models.ps1 is the Windows half and reads the same table.
#
#   scripts/fetch_models.sh              everything (about 5.4 GB)
#   scripts/fetch_models.sh --only stt   only the rows whose dest starts with "stt"
#   scripts/fetch_models.sh --check      verify what is there; download nothing
#
# The app finds these beside its working directory: models/embeddinggemma-300m-q8_0.gguf
# (LlamaEmbedder::find_weights), models/qwen3-tts-0.6b (CloneEngine::find_model), and the
# installer's launcher passes the one .gguf under models/llm and the one folder under
# models/stt by name (installer/Moonshine.ps1).
set -euo pipefail
cd "$(dirname "$0")/.."

only=""
check=0
while (( $# )); do
    case "$1" in
        --only) only="$2"; shift 2 ;;
        --check) check=1; shift ;;
        *) echo "usage: $0 [--only PREFIX] [--check]" >&2; exit 2 ;;
    esac
done

sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
size_of() { stat -f %z "$1" 2>/dev/null || stat -c %s "$1"; }

# A notice before the download, not a click-through: Gemma's weights come under Google's
# terms rather than an open-source licence, and the person fetching them should know.
if [[ -z "$only" || "embeddinggemma" == "$only"* ]] && (( ! check )); then
    echo "embeddinggemma-300m is distributed under the Gemma Terms of Use"
    echo "(https://ai.google.dev/gemma/terms); fetching it means accepting them."
fi

missing=0
wrong=0
while IFS=$'\t' read -r dest url bytes want; do
    [[ -z "$dest" || "$dest" == \#* ]] && continue
    [[ -n "$only" && "$dest" != "$only"* ]] && continue
    path="models/$dest"
    if [[ -f "$path" && "$(size_of "$path")" == "$bytes" ]]; then
        if [[ "$(sha256 "$path")" == "$want" ]]; then
            echo "  ok       $dest"
            continue
        fi
        echo "  wrong    $dest (sha256 differs)"
    elif [[ -f "$path" ]]; then
        echo "  wrong    $dest ($(size_of "$path") bytes, want $bytes)"
    else
        echo "  missing  $dest"
    fi
    if (( check )); then missing=$((missing + 1)); continue; fi
    echo "=== fetching $dest ($(( bytes / 1000000 )) MB) ==="
    mkdir -p "$(dirname "$path")"
    curl -fL --retry 3 --progress-bar -o "$path.part" "$url"
    have=$(sha256 "$path.part")
    if [[ "$have" != "$want" ]]; then
        echo "$dest: downloaded sha256 $have, want $want; left as $path.part" >&2
        wrong=$((wrong + 1))
        continue
    fi
    mv "$path.part" "$path"
done < scripts/models.tsv

if (( check && missing )); then echo "$missing file(s) missing or wrong; run without --check" >&2; exit 1; fi
if (( wrong )); then exit 1; fi
echo "models are in place under $PWD/models"
