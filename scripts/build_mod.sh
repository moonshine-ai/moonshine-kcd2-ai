#!/usr/bin/env bash
# Build the mod into a folder: the generator, the generated files, the checks that say
# they would work, and -- where the game is -- the level's scheduler override. Nothing is
# sent anywhere; the folder is what goes under the game's mods/ (README.md, Installing).
#
#   scripts/build_mod.sh OUT_DIR                        the mod folder, written into OUT_DIR
#   scripts/build_mod.sh OUT_DIR --game /path/to/KingdomComeDeliverance2
#                                                       ...with the scheduler override, read
#                                                       out of that install's own level.pak
#   MOD_VERSION=0.9.1 MOD_DESCRIPTION='...' scripts/build_mod.sh OUT_DIR
#                                                       what the manifest says of itself
#
# Reads data/kcd2/npcs.sqlite (scripts/fetch_drop.sh assembles it; MOONSHINE_DB names
# another) and two sets of assets: the hand-authored ones in mod/assets/pak and the
# generated ones the drop carries (drop/<name>/assets/pak: the facial clips, the costume's
# tables, the Lua the generator does not write). The generator takes one assets folder, so
# the two are laid together first.
#
# The scheduler override is an edit of the level's own 63 MB table, and the table is the
# game's: a patch changes it, so the pak must be built against the install it ships to.
# --game (or MOONSHINE_GAME) says where that is; without it the step is skipped, with a
# warning, and the mod works but a quest that changes its giver's day does not move her.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out=""
game="${MOONSHINE_GAME:-}"
while (( $# )); do
    case "$1" in
        --game) game="$2"; shift 2 ;;
        --game=*) game="${1#--game=}"; shift ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) out="$1"; shift ;;
    esac
done
[[ -n "$out" ]] || { echo "usage: scripts/build_mod.sh OUT_DIR [--game KCD2_DIR]" >&2; exit 2; }
# An absolute path, since the generator is run from the repo root.
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
db="${MOONSHINE_DB:-$here/data/kcd2/npcs.sqlite}"
[[ -f "$db" ]] || { echo "no database at $db; scripts/fetch_drop.sh assembles one" >&2; exit 1; }

cd "$here"

# The drop's name, for its assets.
drop=$(sed -n 's/^drop=//p' data/DROP)
drop_assets="drop/$drop/assets/pak"
[[ -d "$drop_assets" ]] || { echo "no $drop_assets; scripts/fetch_drop.sh brings the drop" >&2; exit 1; }

echo "=== building the generator ==="
cmake -S mod -B mod/build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build mod/build -j8 >/dev/null

# The two asset trees, together. A file in both is the hand-authored one.
assets=$(mktemp -d)
trap 'rm -rf "$assets"' EXIT
cp -R "$drop_assets/." "$assets/"
cp -R mod/assets/pak/. "$assets/"

# --dialogue is what the shipped mod is built with, and what makes these souls talkable
# at all: without it they keep their names and have nothing to say. The manifest's version
# is the release's (scripts/release_version.sh) unless the caller says otherwise.
echo "=== generating ==="
mkdir -p "$out"
version="${MOD_VERSION:-$(scripts/release_version.sh)}"
manifest_args=(--version "$version")
[[ -n "${MOD_DESCRIPTION:-}" ]] && manifest_args+=(--description "$MOD_DESCRIPTION")
./mod/build/kcd2modgen --db "$db" --out "$out" --dialogue \
    --assets "$assets" --assets-text mod/assets/text.tsv \
    "${manifest_args[@]}" \
    | sed 's/^/  /'

# The emitted Lua is one 7,000-line file built by string concatenation, and a syntax
# error in it reaches the game as a mod that loaded and then did nothing. luac is the
# cheapest place to find that out.
if command -v luac >/dev/null; then
    lua_dir=$(mktemp -d)
    ( cd "$lua_dir" && unzip -o -q "$out/Data/moonshine_names.pak" )
    luac -p "$lua_dir/Scripts/mods/moonshine_names.lua"
    echo "  emitted Lua parses"
    # And runs: the harness under mod/harness/ plays the emitted Lua against a stub of
    # the engine (directives, the quest commands, the store's save and load, the menu, a
    # whole errand). A case that fails here is a mod that would misbehave in the game.
    if command -v luajit >/dev/null; then
        luajit mod/harness/run.lua "$lua_dir/Scripts/mods/moonshine_names.lua" \
            | sed -n '/^FAIL/,/^      [^|]/p;$p' | sed 's/^/  /'
        test "${PIPESTATUS[0]}" -eq 0
    else
        echo "  (luajit not installed, harness skipped: brew install luajit)"
    fi
    rm -rf "$lua_dir"
else
    echo "  (luac not installed, Lua checks skipped: brew install lua luajit)"
fi

# The level's scheduler override, for a quest that changes its giver's day. Written only
# when some quest wants it, and only against a game install.
echo "=== scheduler override ==="
override=Data/Levels/kutnohorsko/moonshine_names.pak
rm -f "$out/$override"
if [[ -n "$game" ]]; then
    python3 scripts/build_scheduler.py --db "$db" --game "$game" --out "$out/$override"
else
    echo "  skipped: no --game; the giver of a quest with a routine keeps her usual day" >&2
fi

# The game's word of the mod's own mouth-shape clips (scripts/build_mouthpak.py): the
# install's own Animations/Animations.img with a header per clip appended, since the game
# knows a clip only through that image. Only with MOONSHINE_MOUTH_SHAPES=1, the app's
# MOONSHINE_MOUTH_SHAPES=1 being what sends the shapes: the game's own clips are the mouth
# by default, and an image override shipped for nothing is a game patch waiting to be
# masked.
echo "=== mouth clips in the animation image ==="
mouth=Data/moonshine_mouth.pak
rm -f "$out/$mouth"
if [[ "${MOONSHINE_MOUTH_SHAPES:-0}" == "1" ]]; then
    [[ -n "$game" ]] || { echo "MOONSHINE_MOUTH_SHAPES=1 needs --game" >&2; exit 1; }
    python3 scripts/build_mouthpak.py --clips "$assets/Animations/humans/facials/moonshine" \
        --game "$game" --out "$out/$mouth"
else
    echo "  not built: the game's own clips are the mouth (MOONSHINE_MOUTH_SHAPES=1 for the shapes)"
fi
echo "mod: $out (version $version)"
