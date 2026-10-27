#!/usr/bin/env bash
# Moonshine Voice, for the ear (and Kokoro, where wanted): clone the moonshine tree at its
# pin into third_party/moonshine and build the library the app links (app/CMakeLists.txt,
# MOONSHINE_ROOT). macOS and Linux; scripts/fetch_moonshine.ps1 is the Windows half.
#
#   scripts/fetch_moonshine.sh            clone (or fetch) at the pin, build libmoonshine
#   scripts/fetch_moonshine.sh --no-build  the checkout only
#
# The pin is a commit on the moonshine repository (MOONSHINE_PIN below); moving it is a
# change to this file, so a build says which Moonshine Voice it was made with. The library
# is built from source rather than taken from a release because the app links the shared
# library with ONNX Runtime found by rpath beside it, which is the checkout's layout; the
# release archives carry a static library only.
#
# The tree keeps its ONNX Runtime binaries and two generated source files in Git LFS. Those
# are pulled after the clone; when GitHub's LFS quota for the repository is spent (it says
# "exceeded its LFS budget"), MOONSHINE_LFS_FROM=/path/to/another/moonshine/checkout takes
# the objects out of that clone's own store instead. MOONSHINE_REPO may likewise be a local
# path.
set -euo pipefail
cd "$(dirname "$0")/.."

MOONSHINE_REPO="${MOONSHINE_REPO:-https://github.com/moonshine-ai/moonshine}"
MOONSHINE_PIN="${MOONSHINE_PIN:-9b05faab8adf4367e3c81bf05cd49207d3788e8e}"
root="third_party/moonshine"
build=1
[[ "${1:-}" == "--no-build" ]] && build=0

if [[ ! -d "$root/.git" ]]; then
    echo "=== cloning moonshine into $root ==="
    # Pointers only for now; the LFS objects come next, by whichever way works.
    GIT_LFS_SKIP_SMUDGE=1 git clone --filter=blob:none "$MOONSHINE_REPO" "$root"
fi
have=$(git -C "$root" rev-parse HEAD)
if [[ "$have" != "$MOONSHINE_PIN" ]]; then
    echo "=== checking out $MOONSHINE_PIN ==="
    git -C "$root" fetch --quiet origin "$MOONSHINE_PIN"
    GIT_LFS_SKIP_SMUDGE=1 git -C "$root" checkout --quiet "$MOONSHINE_PIN"
fi
git -C "$root" log --oneline -1

# The LFS objects the build on this platform reads: ORT for it, and the two generated
# sources. A pointer file is a few lines starting "version https://git-lfs"; a real one is
# megabytes, so the size says which is which.
needed() {
    git -C "$root" lfs ls-files -n | grep -E 'core/cpp-annote/|core/moonshine-tts/src/|core/third-party/onnxruntime/lib/(macos|linux)/'
}
missing=()
while IFS= read -r f; do
    if [[ ! -f "$root/$f" ]] || (( $(stat -f %z "$root/$f" 2>/dev/null || stat -c %s "$root/$f") < 1024 )); then
        missing+=("$f")
    fi
done < <(needed)
if (( ${#missing[@]} )); then
    echo "=== pulling ${#missing[@]} LFS objects ==="
    if [[ -n "${MOONSHINE_LFS_FROM:-}" ]]; then
        # From that checkout's object store, or, when git lfs prune has emptied it, from its
        # working tree where the file is the same object (the sha256 is the oid).
        from="$MOONSHINE_LFS_FROM/.git/lfs/objects"
        mkdir -p "$root/.git/lfs/objects"
        for f in "${missing[@]}"; do
            oid=$(git -C "$root" lfs ls-files -l -I "$f" | awk '{print $1}')
            src="$from/${oid:0:2}/${oid:2:2}/$oid"
            if [[ ! -f "$src" ]]; then
                tree="$MOONSHINE_LFS_FROM/$f"
                if [[ -f "$tree" && "$(shasum -a 256 "$tree" | awk '{print $1}')" == "$oid" ]]; then
                    src="$tree"
                else
                    echo "$MOONSHINE_LFS_FROM has no object for $f" >&2; exit 1
                fi
            fi
            mkdir -p "$root/.git/lfs/objects/${oid:0:2}/${oid:2:2}"
            cp "$src" "$root/.git/lfs/objects/${oid:0:2}/${oid:2:2}/$oid"
        done
        git -C "$root" lfs checkout "${missing[@]}"
    elif ! git -C "$root" lfs pull --include "$(IFS=,; echo "${missing[*]}")"; then
        echo "git lfs pull failed. If GitHub says the repository exceeded its LFS budget," >&2
        echo "set MOONSHINE_LFS_FROM to a moonshine checkout that has the objects and run again." >&2
        exit 1
    fi
fi

if (( build )); then
    echo "=== building libmoonshine ==="
    cmake -S "$root/core" -B "$root/core/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$root/core/build" --target moonshine -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
    ls -la "$root"/core/build/libmoonshine.*
fi
echo "app/CMakeLists.txt finds it at MOONSHINE_ROOT=$PWD/$root"
