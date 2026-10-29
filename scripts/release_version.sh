#!/usr/bin/env bash
# The version a tester build carries, the same on the mod's manifest, the installer's
# name and wherever the files are published, so a log that says one says all three.
#
# Sourced by the release scripts; or run, to print it. It is the one line of VERSION at
# the root of the checkout, which scripts/release.sh bumps and commits with each release
# (a file, rather than a count of commits, because the Windows build machine's copy of
# the tree is not a git checkout and the count had drifted from what testers were sent).
# 0.9 because these are tester builds of a mod that has not shipped. MOONSHINE_VERSION
# names one outright.
#
#   scripts/release_version.sh            0.9.97
#   MOONSHINE_VERSION=1.0.0 scripts/release_version.sh

release_version() {
    if [[ -n "${MOONSHINE_VERSION:-}" ]]; then
        echo "$MOONSHINE_VERSION"
        return
    fi
    local here
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    if [[ ! -f "$here/VERSION" ]]; then
        echo "no VERSION file at $here" >&2
        return 1
    fi
    tr -d '[:space:]' < "$here/VERSION"
    echo
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    release_version
fi
