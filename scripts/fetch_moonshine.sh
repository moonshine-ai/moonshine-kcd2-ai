#!/usr/bin/env bash
# Moonshine Voice, the prebuilt library: download the release archive for this machine into
# third_party/moonshine-voice (include/ and lib/) and check it against its pinned sha256.
# Nothing is compiled. The app links it (app/CMakeLists.txt, MOONSHINE_ROOT) for the ear,
# for Kokoro where that is wanted, and for the voices: Phonon is in the library.
# macOS and Linux; scripts/fetch_moonshine.ps1 is the Windows half.
#
#   scripts/fetch_moonshine.sh            download and unpack, unless it is already there
#   scripts/fetch_moonshine.sh --force    download and unpack again
#
# The pin is a release tag on github.com/moonshine-ai/moonshine (MOONSHINE_VERSION below);
# moving it is an edit to this file and to the hashes under it, so a build says which
# Moonshine Voice it was made with.
#
# What the archives hold differs by platform, and app/CMakeLists.txt deals with each:
#   macOS arm64    lib/libmoonshine.a, static, with ONNX Runtime merged in
#   linux x86_64   lib/libmoonshine.so and lib/libonnxruntime.so.1, found by $ORIGIN rpath
#   linux arm64    the same
#   windows        see fetch_moonshine.ps1
# There is no Intel macOS archive in the release; the library has to be built from source
# there (core/ in the moonshine repository), and MOONSHINE_ROOT pointed at an unpacked tree
# of the same layout.
set -euo pipefail
cd "$(dirname "$0")/.."

MOONSHINE_VERSION="${MOONSHINE_VERSION:-v0.1.6}"
MOONSHINE_RELEASE_URL="${MOONSHINE_RELEASE_URL:-https://github.com/moonshine-ai/moonshine/releases/download/$MOONSHINE_VERSION}"

force=0
[[ "${1:-}" == "--force" ]] && force=1

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)
        platform=macos-arm64
        want=809555a4e2c9a6972c0960a4cbf08b95121763ca17bce69f817adaac3b804fd0 ;;
    Linux-x86_64)
        platform=linux-x86_64
        want=0e6074838d47f174b0188533f1819d502f376e77dccb771df5c09380737b76ec ;;
    Linux-aarch64|Linux-arm64)
        platform=linux-arm64
        want=1e0a88f27c428cab43590ce1635fe122406ef9e70236b58d077daf4b95cf162c ;;
    *)
        echo "no Moonshine Voice $MOONSHINE_VERSION archive for $(uname -s) $(uname -m)." >&2
        echo "Build core/ from the moonshine repository and point MOONSHINE_ROOT at a folder" >&2
        echo "with include/ and lib/ in it." >&2
        exit 1 ;;
esac
# A pin that is moved without its hash is not the pin it was: the hash only holds for v0.1.6.
if [[ "$MOONSHINE_VERSION" != "v0.1.6" && -z "${MOONSHINE_SHA256:-}" ]]; then
    echo "MOONSHINE_VERSION=$MOONSHINE_VERSION needs MOONSHINE_SHA256 for its $platform archive." >&2
    exit 1
fi
want="${MOONSHINE_SHA256:-$want}"

sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

root="third_party/moonshine-voice"
stamp="$root/.release"
if (( ! force )) && [[ -f "$stamp" && "$(cat "$stamp")" == "$MOONSHINE_VERSION $platform" ]] \
        && [[ -f "$root/include/moonshine-cpp.h" ]]; then
    echo "Moonshine Voice $MOONSHINE_VERSION ($platform) is already in $root"
    exit 0
fi

archive="moonshine-voice-$platform.tar.gz"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "=== fetching $archive ($MOONSHINE_VERSION) ==="
curl -fL --retry 3 --progress-bar -o "$tmp/$archive" "$MOONSHINE_RELEASE_URL/$archive"
have="$(sha256 "$tmp/$archive")"
if [[ "$have" != "$want" ]]; then
    echo "$archive: sha256 $have, want $want" >&2
    exit 1
fi

# Replace whatever was there, so a library from an earlier pin cannot be linked beside this
# one's headers.
rm -rf "$root"
mkdir -p "$root"
tar xzf "$tmp/$archive" -C "$root" --strip-components=1
echo "$MOONSHINE_VERSION $platform" > "$stamp"
ls -la "$root/lib"
echo "app/CMakeLists.txt finds it at MOONSHINE_ROOT=$PWD/$root"
