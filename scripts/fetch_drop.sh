#!/usr/bin/env bash
# The pinned data drop (data/DROP), into drop/ and assembled into data/kcd2/. All of the
# work is tools/fetch_drop.py; this finds a Python for it.
#
#   scripts/fetch_drop.sh --from DIR|gs://...|https://...   (or MOONSHINE_DROP_BASE)
#   scripts/fetch_drop.sh --check | --assemble
set -euo pipefail
cd "$(dirname "$0")/.."
py="${PYTHON:-}"
[[ -n "$py" ]] || py=$(command -v python3 || command -v python)
exec "$py" tools/fetch_drop.py "$@"
