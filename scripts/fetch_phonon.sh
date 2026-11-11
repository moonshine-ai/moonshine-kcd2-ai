#!/usr/bin/env bash
# The Phonon voice model, MADE FROM SOURCE, into models/phonon. You normally do not want
# this: scripts/fetch_models.sh --only phonon downloads the same folder from Moonshine's
# server, pinned by sha256 in scripts/models.tsv. This is for when that server's copy is
# not there or not what you want (a newer Phonon, a changed voice), and what it writes
# will not match those pins, so installer/Stage.ps1 refuses it until the pins are moved.
#
# The folder is the six ONNX Runtime graphs, tokenizer.bin, voices.json and one
# voices/<name>.safetensors per speaker, in the layout the Moonshine framework's C++
# Phonon engine (and so PhononEngine, phonon.cpp) loads.
#
#   scripts/fetch_phonon.sh            export it if it is not already there
#   scripts/fetch_phonon.sh --force    export it again
#   scripts/fetch_phonon.sh --check    say whether it is there; export nothing
#
# It uses upstream's exporter (scripts/export-phonon-model.py, which is not in the v0.1.6
# release tag, hence the pinned commit below). It pulls Gradium's Apache-2.0
# weights from Hugging Face (gradium/phonon-31m-en, phonon-mimi, phonon-voices), checks
# every graph against the PyTorch model, and writes .ort files. It needs python3 (3.10 to
# 3.12), git, about 3 GB of disk for torch and a few minutes. Nothing is installed outside
# a throwaway venv under build/phonon-export.
#
# The app looks for the folder in PHONON_MODEL_DIR, models/phonon, and then the
# moonshine_voice cache (PhononEngine::find_model). It must be named "phonon"; the engine
# reads <parent>/phonon/....
set -euo pipefail
cd "$(dirname "$0")/.."

MOONSHINE_SRC_URL="${MOONSHINE_SRC_URL:-https://github.com/moonshine-ai/moonshine.git}"
MOONSHINE_SRC_REV="${MOONSHINE_SRC_REV:-d8d8fa29d05ca67441a7719e70099f9f48917a93}"
out=models/phonon

force=0
check=0
for a in "$@"; do
    case "$a" in
        --force) force=1 ;;
        --check) check=1 ;;
        *) echo "usage: $0 [--force|--check]" >&2; exit 2 ;;
    esac
done

complete() {
    local d="$1"
    for f in backbone.ort embed_latent.ort embed_prompt.ort flow_head.ort \
             mimi_seanet.ort mimi_transformer.ort model.json tokenizer.bin voices.json; do
        [[ -s "$d/$f" ]] || return 1
    done
    compgen -G "$d/voices/*.safetensors" >/dev/null
}

if complete "$out" && (( ! force )); then
    echo "  ok       $out ($(ls "$out/voices" | wc -l | tr -d ' ') voices)"
    exit 0
fi
if (( check )); then echo "  missing  $out; run scripts/fetch_phonon.sh" >&2; exit 1; fi

work=build/phonon-export
mkdir -p "$work"

if [[ ! -f "$work/src/scripts/export-phonon-model.py" ]]; then
    rm -rf "$work/src"
    git clone --quiet --filter=blob:none --sparse "$MOONSHINE_SRC_URL" "$work/src"
    git -C "$work/src" sparse-checkout set scripts
    git -C "$work/src" checkout --quiet "$MOONSHINE_SRC_REV"
fi

if [[ ! -x "$work/venv/bin/python" ]]; then
    python3 -m venv "$work/venv"
    "$work/venv/bin/python" -m pip install --quiet --upgrade pip
    "$work/venv/bin/python" -m pip install --quiet \
        torch "transformers>=5.21" onnx onnxruntime safetensors numpy huggingface_hub
fi

rm -rf "$out.part"
"$work/venv/bin/python" "$work/src/scripts/export-phonon-model.py" --out-dir "$out.part"
# reference.f32 is the exporter's test vector, not something the engine reads.
rm -f "$out.part/reference.f32"
if ! complete "$out.part"; then
    echo "the export finished but $out.part is incomplete" >&2
    exit 1
fi
rm -rf "$out"
mv "$out.part" "$out"
echo "phonon is in place under $PWD/$out"
