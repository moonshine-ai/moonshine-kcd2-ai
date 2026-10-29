#!/usr/bin/env bash
# A tester release, start to finish, from this checkout:
#
#   scripts/release.sh                       the next number (VERSION's last part + 1)
#   scripts/release.sh --version 1.0.0       that number
#   scripts/release.sh --stage-only          everything but the upload (to run the
#                                            verifiers against release\out first); then
#   scripts/release.sh --publish-only --version 0.9.98
#
# What it does, in order, stopping at the first thing that fails:
#
#   1. Bumps VERSION, and the installer link in INSTALL.md, in the working tree. The
#      tree has to be clean apart from that (--dirty to release uncommitted work anyway),
#      so the tag at the end names what was shipped.
#   2. Copies the tracked files of this repository (not the submodules, which the build
#      machine has checked out already and which change rarely) over ssh to the Windows
#      build machine, with tar, so it builds what is committed here and nothing else.
#   3. On the build machine: scripts\build_windows.bat release (the CPU-only build and
#      the tests), then installer\Stage.ps1, which assembles the package, writes the
#      manifest and builds the installer with Inno Setup -- signed when that machine has
#      installer\signing.json and an `az login`, unsigned with a note otherwise (--no-sign
#      to leave the signing out on purpose).
#   4. On the build machine again: installer\publish.py, which uploads what the bucket
#      lacks (the files are named by their hashes, so an unchanged database or mod costs
#      nothing), then the installer and manifest, then latest/manifest.json, which is
#      what every installed copy's Update looks at. The R2 credentials are read from this
#      machine's environment (CLOUDFLARE_ACCOUNT_ID, CLOUDFLARE_ACCESS_KEY_ID,
#      CLOUDFLARE_SECRET_ACCESS_KEY, or S3_ENDPOINT / AWS_ACCESS_KEY_ID /
#      AWS_SECRET_ACCESS_KEY) and handed to the script on its standard input, over the
#      ssh connection; they are never on a command line or on the build machine's disk.
#   5. Commits VERSION and INSTALL.md as "Release <version>" and tags it v<version>.
#      Nothing is pushed; that is yours to do when you have seen it work.
#
# If anything fails before the upload, VERSION and INSTALL.md are put back so the next
# attempt gets the same number; after the upload has started the number is spent and the
# files are left bumped for you to finish with --publish-only.
#
# The build machine is RELEASE_HOST (wl1) and the checkout on it RELEASE_CHECKOUT
# (kcd2ai, under the ssh user's home); its python is RELEASE_PYTHON (py -3). The files are
# published under MOONSHINE_DOWNLOAD_BASE (https://download.moonshine.ai/kcd2). The
# release build's prerequisites on that machine -- the drop, the models, release\mod --
# are its own (README.md, The installer) and are checked by Stage.ps1, not here.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$here"
host="${RELEASE_HOST:-wl1}"
checkout="${RELEASE_CHECKOUT:-kcd2ai}"
python="${RELEASE_PYTHON:-py -3}"
base="${MOONSHINE_DOWNLOAD_BASE:-https://download.moonshine.ai/kcd2}"
base="${base%/}"
version=""
stage_only=0
publish_only=0
dirty=0
nosign=""
while (( $# )); do
    case "$1" in
        --version) version="$2"; shift 2 ;;
        --version=*) version="${1#--version=}"; shift ;;
        --stage-only) stage_only=1; shift ;;
        --publish-only) publish_only=1; shift ;;
        --dirty) dirty=1; shift ;;
        --no-sign) nosign="-NoSign"; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1 (--help)" >&2; exit 2 ;;
    esac
done
if (( stage_only && publish_only )); then echo "--stage-only and --publish-only are each other's other half" >&2; exit 2; fi
if (( publish_only )) && [[ -z "$version" ]]; then echo "--publish-only wants --version, the number that was staged" >&2; exit 2; fi

step() { printf '\n=== %s\n' "$*"; }
# A command on the build machine, in the checkout. Windows OpenSSH runs cmd.exe, so the
# paths are backslashed and the chaining is cmd's.
remote() { ssh "$host" "cd $checkout && $*"; }

# --- credentials, before anything is bumped ----------------------------------------------------
endpoint="${S3_ENDPOINT:-}"
if [[ -z "$endpoint" && -n "${CLOUDFLARE_ACCOUNT_ID:-}" ]]; then endpoint="https://$CLOUDFLARE_ACCOUNT_ID.r2.cloudflarestorage.com"; fi
access_key="${AWS_ACCESS_KEY_ID:-${CLOUDFLARE_ACCESS_KEY_ID:-}}"
secret_key="${AWS_SECRET_ACCESS_KEY:-${CLOUDFLARE_SECRET_ACCESS_KEY:-}}"
if (( ! stage_only )) && [[ -z "$endpoint" || -z "$access_key" || -z "$secret_key" ]]; then
    echo "no R2 credentials in the environment: CLOUDFLARE_ACCOUNT_ID, CLOUDFLARE_ACCESS_KEY_ID and CLOUDFLARE_SECRET_ACCESS_KEY (or S3_ENDPOINT, AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY); or --stage-only" >&2
    exit 1
fi

publish() {
    step "publishing $version to $base"
    printf '%s\n%s\n%s\n' "$endpoint" "$access_key" "$secret_key" |
        remote "$python installer\\publish.py --version $version --download-base $base --creds-stdin"
}

if (( publish_only )); then
    publish
    echo
    echo "published: $base/$version/Moonshine-$version-setup.exe"
    if git diff --quiet -- VERSION INSTALL.md; then
        echo "VERSION and INSTALL.md are committed already"
    else
        git commit -q -m "Release $version" -- VERSION INSTALL.md && git tag -a "v$version" -m "Moonshine $version" && echo "committed and tagged v$version (not pushed)"
    fi
    exit 0
fi

# --- the number ----------------------------------------------------------------------------------
step 'version'
# Uncommitted changes to tracked files, or a moved submodule; what is loose inside a
# submodule's own tree does not count, since the build machine has its own checkouts.
if ! git diff --quiet --ignore-submodules=dirty || ! git diff --cached --quiet --ignore-submodules=dirty; then
    if (( dirty )); then
        echo "  the working tree has uncommitted changes (--dirty: releasing them anyway)"
    else
        git status --short --untracked-files=no --ignore-submodules=dirty
        echo "  the working tree has uncommitted changes: commit them, or --dirty to release them anyway" >&2
        exit 1
    fi
fi
current="$(tr -d '[:space:]' < VERSION)"
if [[ -z "$version" ]]; then
    [[ "$current" =~ ^([0-9]+(\.[0-9]+)*\.)([0-9]+)$ ]] || { echo "VERSION holds '$current', not a dotted number" >&2; exit 1; }
    version="${BASH_REMATCH[1]}$(( BASH_REMATCH[3] + 1 ))"
fi
[[ "$version" =~ ^[0-9]+(\.[0-9]+)+$ ]] || { echo "'$version' is not a version (digits and dots)" >&2; exit 1; }
if git rev-parse -q --verify "refs/tags/v$version" >/dev/null; then echo "v$version is tagged already" >&2; exit 1; fi
echo "  $current -> $version"
printf '%s\n' "$version" > VERSION
# INSTALL.md's one link to the installer, whatever version it named.
sed -E "s#kcd2/[0-9.]+/Moonshine-[0-9.]+-setup\.exe#kcd2/$version/Moonshine-$version-setup.exe#g" INSTALL.md > INSTALL.md.tmp && mv INSTALL.md.tmp INSTALL.md
spent=0
restore() {
    if (( ! spent )); then
        git checkout -q -- VERSION INSTALL.md
        echo "VERSION and INSTALL.md put back to $current" >&2
    else
        echo "the upload of $version had begun; VERSION and INSTALL.md are left at $version -- finish with: scripts/release.sh --publish-only --version $version" >&2
    fi
}
trap 'restore' ERR

# --- the source, over to the build machine ----------------------------------------------------
step "source to $host:$checkout"
# The tracked files as a tar stream, less the submodules (git lists each as its path, and
# tar would pack the whole checkout under it); mtimes travel with the files, so one that
# has not changed does not make MSVC rebuild its object.
excludes=()
while IFS= read -r sub; do excludes+=(--exclude "$sub"); done < <(git config --file .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}')
count=$(git ls-files -s | awk '$1 != "160000"' | wc -l | tr -d ' ')
git ls-files -z | tar -czf - --null -T - ${excludes[@]+"${excludes[@]}"} |
    ssh "$host" "tar -xzf - -C $checkout"
echo "  $count files"

# --- build, stage, installer --------------------------------------------------------------------
step "building on $host (scripts\\build_windows.bat release)"
log="$(mktemp -t moonshine-release-build)"
if remote 'scripts\build_windows.bat release' > "$log" 2>&1; then
    grep -E '^\[doctest\] (test cases|Status)' "$log" | sed 's/^/  /' || true
    rm -f "$log"
else
    tail -n 40 "$log" >&2
    echo "the build failed (the whole log is $log)" >&2
    false
fi

step "staging $version on $host (installer\\Stage.ps1)"
remote "powershell -NoProfile -ExecutionPolicy Bypass -File installer\\Stage.ps1 -Version $version -DownloadBase $base $nosign"

if (( stage_only )); then
    trap - ERR
    echo
    echo "staged, not published. On $host: $checkout\\release\\out\\$version\\Moonshine-$version-setup.exe,"
    echo "with $checkout\\release\\stage\\Moonshine as /MIRROR for the verifiers. VERSION and INSTALL.md are"
    echo "left at $version, uncommitted; when it is good:  scripts/release.sh --publish-only --version $version"
    exit 0
fi

# --- up, then the commit --------------------------------------------------------------------------
spent=1
publish
trap - ERR
step 'commit'
git commit -q -m "Release $version" -- VERSION INSTALL.md
git tag -a "v$version" -m "Moonshine $version"
echo "  Release $version, tagged v$version (not pushed)"
echo
echo "published: $base/$version/Moonshine-$version-setup.exe"
