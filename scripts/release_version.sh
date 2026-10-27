#!/usr/bin/env bash
# The version a tester build carries, the same on the mod's manifest, the installer's
# name and wherever the files are published, so a log that says one says all three.
#
# Sourced by the release scripts; or run, to print it. MOONSHINE_VERSION names one
# outright. Otherwise 0.9.<N>, with N the number of commits on HEAD plus a base carried
# over from the repository this one was split out of (its count stood at 95 when the
# first commit here was made), so the numbers testers have seen keep going up: monotonic
# without a file to bump and forget, and 0.9 because these are tester builds of a mod
# that has not shipped.
#
#   scripts/release_version.sh            0.9.96
#   MOONSHINE_VERSION=1.0.0 scripts/release_version.sh

release_version() {
    if [[ -n "${MOONSHINE_VERSION:-}" ]]; then
        echo "$MOONSHINE_VERSION"
        return
    fi
    local here count base=95
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    count=$(git -C "$here" rev-list --count HEAD 2>/dev/null || echo 0)
    echo "0.9.$((base + count))"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    release_version
fi
