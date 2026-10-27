#!/usr/bin/env bash
# The version a tester drop carries, the same on the mod's manifest, the installer's
# name and the bucket folder, so a log that says one says all three.
#
# Sourced by release_mod.sh and release_stage.sh; or run, to print it. MOONSHINE_VERSION
# names one outright. Otherwise 0.9.<commits on HEAD>: monotonic without a file to bump
# and forget, and 0.9 because these are tester builds of a mod that has not shipped.
#
#   scripts/release_version.sh            0.9.312
#   MOONSHINE_VERSION=0.9.5 scripts/release_version.sh

release_version() {
    if [[ -n "${MOONSHINE_VERSION:-}" ]]; then
        echo "$MOONSHINE_VERSION"
        return
    fi
    local here count
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    count=$(git -C "$here" rev-list --count HEAD 2>/dev/null || echo 0)
    echo "0.9.$count"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    release_version
fi
