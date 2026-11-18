#!/usr/bin/env bash
# Checks the bug report service from the outside, the way the app uses it.
#
#   ./smoke.sh                       against https://reports.moonshine.ai
#   ./smoke.sh http://localhost:8787 against `wrangler dev`
#   ./smoke.sh --link <url>          the download link from the email it sent
#   ./smoke.sh --rate [url]          the per-minute limit too (sends nothing real)
#
# The first form sends ONE real report, so one email arrives and one small zip
# sits in the bucket for 90 days. The link in that email is the only way to the
# zip, which is the point, so checking the download is the second step.
set -euo pipefail

base="https://reports.moonshine.ai"
link=""
rate=0
while [ $# -gt 0 ]; do
    case "$1" in
        --link) link="${2:?--link needs the URL from the email}"; shift 2 ;;
        --rate) rate=1; shift ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) base="${1%/}"; shift ;;
    esac
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failed=0
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; failed=1; }
# expect <what> <wanted status> <got status>
expect() { if [ "$3" = "$2" ]; then ok "$1 ($3)"; else bad "$1: wanted $2, got $3: $(head -c 300 "$work/answer" 2>/dev/null)"; fi; }

# base64 of the form, on one line, as the app sends it.
form() { printf '%s' "$1" | base64 | tr -d '\n'; }

if [ -n "$link" ]; then
    echo "downloading $link"
    status="$(curl -sS -o "$work/back.zip" -w '%{http_code}' "$link")"
    expect "the link in the email downloads" 200 "$status"
    if unzip -p "$work/back.zip" smoke.txt 2>/dev/null | grep -q '^smoke test'; then
        ok "it is the zip that was sent"
    else
        bad "what came back is not the zip that was sent"
    fi
    status="$(curl -sS -o "$work/answer" -w '%{http_code}' "${link%/logs.zip}/report.json")"
    expect "report.json beside it" 200 "$status"
    wrong="$(printf '%s' "$link" | sed -E 's/-[0-9a-f]{32}\//-00000000000000000000000000000000\//')"
    status="$(curl -sS -o "$work/answer" -w '%{http_code}' "$wrong")"
    expect "a wrong secret opens nothing" 404 "$status"
    exit "$failed"
fi

echo "checking $base"

if [ "$rate" = 1 ]; then
    # Reports with a form that cannot be read are counted by the limiter and then
    # refused, so this stores nothing and sends no email. Cloudflare's limiter is
    # approximate and catches up a few seconds behind (a burst of thirty in three
    # seconds gets through whole), hence the pause and "within 40", not "the
    # seventh". The limits that cannot be slipped past are the daily ones.
    printf 'x' > "$work/one"
    for n in $(seq 1 40); do
        status="$(curl -sS -o "$work/answer" -w '%{http_code}' -X POST \
            -H 'Content-Type: application/zip' -H 'X-Moonshine-Report: !!!' \
            --data-binary "@$work/one" "$base/kcd2/report")"
        if [ "$status" = 429 ]; then ok "refused as too fast at request $n (429)"; exit 0; fi
        if [ "$status" != 400 ]; then bad "request $n: wanted 400 or 429, got $status"; exit 1; fi
        sleep 0.5
    done
    bad "40 reports in 20 seconds and none refused as too fast"
    exit 1
fi

printf 'smoke test %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$work/smoke.txt"
(cd "$work" && zip -q logs.zip smoke.txt)

status="$(curl -sS -o "$work/answer" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/zip' \
    -H "X-Moonshine-Report: $(form '{"name":"Smoke Test","email":"smoke@example.com","description":"smoke.sh: a test of the report service.\nSecond line, with <b>markup</b> & an accent: é.","version":"smoke"}')" \
    --data-binary "@$work/logs.zip" "$base/kcd2/report")"
expect "a small report is taken" 200 "$status"
ref="$(sed -nE 's/.*"ref":"([A-Z2-9]{8})".*/\1/p' "$work/answer")"
if [ -n "$ref" ]; then ok "reference $ref"; else bad "no reference in: $(cat "$work/answer")"; fi
if grep -q '"emailed":true' "$work/answer"; then ok "an email was sent"; else bad "no email was sent: $(cat "$work/answer")"; fi

# One byte over the limit. Refused on its declared size, before it is read.
head -c $((25 * 1024 * 1024 + 1)) /dev/zero > "$work/big"
status="$(curl -sS -o "$work/answer" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/zip' --data-binary "@$work/big" "$base/kcd2/report" || true)"
expect "an oversized report is refused" 413 "$status"

status="$(curl -sS -o "$work/answer" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/zip' -H 'X-Moonshine-Report: !!!' \
    --data-binary "@$work/logs.zip" "$base/kcd2/report")"
expect "a form that cannot be read is refused" 400 "$status"

status="$(curl -sS -o "$work/answer" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/zip' -H 'Content-Length: 0' "$base/kcd2/report")"
expect "an empty report is refused" 400 "$status"

status="$(curl -sS -o "$work/answer" -w '%{http_code}' "$base/kcd2/report")"
expect "GET on the upload address is refused" 405 "$status"

status="$(curl -sS -o "$work/answer" -w '%{http_code}' \
    "$base/kcd2/r/2026-01-01/ABCDEFGH-00000000000000000000000000000000/logs.zip")"
expect "a made-up link opens nothing" 404 "$status"

status="$(curl -sS -o "$work/answer" -w '%{http_code}' "$base/kcd2/")"
expect "no listing" 404 "$status"

if [ "$failed" = 0 ]; then
    echo
    echo "All passed. An email with reference $ref should arrive within a minute;"
    echo "check its link with:  $0 --link '<the link in the email>'"
fi
exit "$failed"
