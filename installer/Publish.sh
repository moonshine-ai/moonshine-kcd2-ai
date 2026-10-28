#!/usr/bin/env bash
# Put a staged release where the installer downloads it from: the R2 bucket behind
# https://download.moonshine.ai, under kcd2/<version>/, then kcd2/latest/manifest.json.
#
#   installer/Publish.sh 0.9.96 [path/to/release/publish]
#
# The publish folder is what installer\Stage.ps1 writes on the Windows build machine
# (release\publish\<version>\ and release\publish\latest\); copy it here first, e.g.
#   scp -r wl1:kcd2ai/release/publish/0.9.96 wl1:kcd2ai/release/publish/latest /tmp/publish/
#
# Needs the aws CLI and, in the environment, CLOUDFLARE_ACCOUNT_ID plus an R2 S3 token as
# CLOUDFLARE_ACCESS_KEY_ID / CLOUDFLARE_SECRET_ACCESS_KEY. Every file is hashed against
# the manifest before anything goes up. The versioned files are immutable (a year's
# cache); latest/manifest.json is what an installed Moonshine.ps1 -Update reads, so it
# gets five minutes and goes up last, after everything it names is there. Reruns are
# cheap: s3 sync moves only what differs.
set -euo pipefail
version="${1:?version, e.g. 0.9.96}"
publish="${2:-/tmp/publish}"
bucket="download-moonshine-ai"
prefix="kcd2"
base="https://download.moonshine.ai/$prefix"
: "${CLOUDFLARE_ACCOUNT_ID:?}" "${CLOUDFLARE_ACCESS_KEY_ID:?}" "${CLOUDFLARE_SECRET_ACCESS_KEY:?}"
export AWS_ACCESS_KEY_ID="$CLOUDFLARE_ACCESS_KEY_ID" AWS_SECRET_ACCESS_KEY="$CLOUDFLARE_SECRET_ACCESS_KEY" AWS_DEFAULT_REGION=auto
unset AWS_PROFILE
ep="https://$CLOUDFLARE_ACCOUNT_ID.r2.cloudflarestorage.com"
tree="$publish/$version"
[ -f "$tree/manifest.json" ] || { echo "no $tree/manifest.json" >&2; exit 1; }

echo "=== checking $tree against its manifest"
python3 - "$tree" "$base/$version/" <<'EOF'
import json, hashlib, os, sys
tree, base = sys.argv[1], sys.argv[2]
m = json.load(open(os.path.join(tree, 'manifest.json'), encoding='utf-8'))
bad = 0; n = 0; total = 0
def check(path, bytes_, sha):
    global bad, total
    if not os.path.exists(path): print('  missing', path); bad += 1; return
    size = os.path.getsize(path); total += size
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    if size != bytes_ or h.hexdigest() != sha: print('  WRONG', path); bad += 1
for f in m['files']:
    if f['url'].startswith(base):
        n += 1; check(os.path.join(tree, f['path']), f['bytes'], f['sha256'])
s = m['setup']
if not s['url'].startswith(base): print('  the installer URL is not under', base); bad += 1
else: check(os.path.join(tree, os.path.basename(s['url'])), s['bytes'], s['sha256'])
latest = os.path.join(os.path.dirname(tree), 'latest', 'manifest.json')
if not os.path.exists(latest) or open(latest, 'rb').read() != open(os.path.join(tree, 'manifest.json'), 'rb').read():
    print('  latest/manifest.json is missing or differs from the versioned one'); bad += 1
print(f'  {n} hosted files + installer, {total/1e9:.2f} GB, {bad} wrong')
sys.exit(1 if bad else 0)
EOF

echo "=== uploading to s3://$bucket/$prefix/$version/"
aws s3 sync "$tree" "s3://$bucket/$prefix/$version" --endpoint-url "$ep" \
    --cache-control "public, max-age=31536000, immutable" --only-show-errors
aws s3 cp "$tree/manifest.json" "s3://$bucket/$prefix/$version/manifest.json" --endpoint-url "$ep" \
    --content-type application/json --cache-control "public, max-age=31536000, immutable" --only-show-errors

echo "=== checking $base/$version/"
curl -fsS "$base/$version/manifest.json" | python3 -c "import json,sys; m=json.load(sys.stdin); assert m['version']=='$version', m['version']; print('  manifest', m['version'], len(m['files']), 'files')"
code=$(curl -s -o /dev/null -w '%{http_code}' -I "$base/$version/Moonshine-$version-setup.exe")
[ "$code" = 200 ] || { echo "  installer not reachable ($code)" >&2; exit 1; }
echo "  installer reachable"

echo "=== publishing latest/manifest.json"
aws s3 cp "$publish/latest/manifest.json" "s3://$bucket/$prefix/latest/manifest.json" --endpoint-url "$ep" \
    --content-type application/json --cache-control "public, max-age=300" --only-show-errors
curl -fsS "$base/latest/manifest.json" | python3 -c "import json,sys; m=json.load(sys.stdin); print('  latest ->', m['version'])"
echo "installer: $base/$version/Moonshine-$version-setup.exe"
