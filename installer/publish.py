#!/usr/bin/env python3
"""Put a staged release where the installer downloads it from.

    py -3 installer\\publish.py --version 0.9.98 [--check] [--no-latest]
        [--repo C:\\path\\to\\checkout] [--download-base https://download.moonshine.ai/kcd2]
        [--bucket download-moonshine-ai] [--creds-stdin]

Runs on the machine that ran installer\\Stage.ps1, over what it wrote: the manifest and the
installer under release\\publish\\<version>\\, and the files the manifest names, read out of
release\\stage\\Moonshine\\. Standard library only (SigV4 through tools/s3_sign.py), so the
Windows build machine's python is enough and nothing has to be copied off it first.

What goes where under the bucket, with <prefix> the path of --download-base (kcd2):

    <prefix>/blobs/<sha256>/<name>        the database, the voices' text files, the mod:
                                          named by their bytes, immutable, cached a year.
                                          Uploaded only when the bucket has no object of
                                          that key and size, so an unchanged file costs
                                          nothing from one release to the next and a
                                          changed one lands beside the old, never over it.
    <prefix>/<version>/manifest.json      this release's manifest, with the installer's hash
    <prefix>/<version>/Moonshine-<version>-setup.exe
    <prefix>/latest/manifest.json         the same manifest, cached five minutes: what an
                                          installed Moonshine.ps1 -Update reads. Goes up
                                          last, once everything it names is reachable.

Every file is hashed against the manifest before anything moves, and nothing is ever
deleted, so each installer handed out keeps finding exactly the bytes its manifest names.
The per-version objects are compared by MD5 (the ETag of a single PUT) and re-uploaded
when they differ, for the case of a release staged twice under one number before it is
out; the blobs are their own hash and need only exist.

Credentials: S3_ENDPOINT (or CLOUDFLARE_ACCOUNT_ID, from which the R2 endpoint follows),
AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY (or CLOUDFLARE_ACCESS_KEY_ID /
CLOUDFLARE_SECRET_ACCESS_KEY); or, with --creds-stdin, those three on three lines of
standard input, which is how scripts/release.sh hands them over an ssh connection without
putting them on a command line. --check does the hashing and the HEADs and reports what
would be uploaded, moving nothing.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from s3_sign import presign  # noqa: E402

IMMUTABLE = "public, max-age=31536000, immutable"
SHORT = "public, max-age=300"
RETRIES = 4


def fail(msg: str) -> None:
    print(msg, file=sys.stderr)
    sys.exit(1)


def mb(n: int) -> str:
    return f"{n / 1e6:,.1f} MB"


def digest(path: str, algo: str) -> str:
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Progress:
    """A file wrapped so that http.client's reads show how far the upload has got."""

    def __init__(self, f, total: int, label: str):
        self.f, self.total, self.label = f, total, label
        self.done = 0
        self.t0 = time.monotonic()
        self.shown = -1.0
        self.tty = sys.stdout.isatty()

    def read(self, n: int = -1) -> bytes:
        data = self.f.read(n)
        self.done += len(data)
        now = time.monotonic()
        if self.tty and now - self.shown >= 0.5:
            rate = self.done / max(now - self.t0, 1e-6) / 1e6
            pct = self.done * 100 // max(self.total, 1)
            print(f"\r  {pct:3d}%  {mb(self.done):>12} / {mb(self.total):<12} {rate:5.1f} MB/s  {self.label}", end="", flush=True)
            self.shown = now
        return data

    def finish(self) -> None:
        if self.tty:
            print("\r" + " " * 100 + "\r", end="", flush=True)


class Store:
    """One bucket on an S3-compatible endpoint, spoken to through presigned URLs."""

    def __init__(self, endpoint: str, bucket: str, access_key: str, secret_key: str):
        self.endpoint, self.bucket = endpoint.rstrip("/"), bucket
        self.ak, self.sk = access_key, secret_key

    def _url(self, method: str, key: str) -> str:
        return presign(method, self.endpoint, self.bucket, key, self.ak, self.sk, expires=3600)

    def head(self, key: str) -> dict[str, str] | None:
        """The object's headers (Content-Length, ETag), or None when there is no such key."""
        req = urllib.request.Request(self._url("HEAD", key), method="HEAD")
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return {k.lower(): v for k, v in resp.headers.items()}
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise RuntimeError(f"HEAD {key}: HTTP {e.code} {e.reason}") from None

    def put(self, path: str, key: str, cache_control: str, content_type: str | None = None) -> None:
        size = os.path.getsize(path)
        content_type = content_type or mimetypes.guess_type(path)[0] or "application/octet-stream"
        for attempt in range(1, RETRIES + 1):
            with open(path, "rb") as f:
                body = Progress(f, size, key.rsplit("/", 1)[-1])
                req = urllib.request.Request(self._url("PUT", key), data=body, method="PUT", headers={
                    "Content-Length": str(size),
                    "Content-Type": content_type,
                    "Cache-Control": cache_control,
                })
                try:
                    with urllib.request.urlopen(req, timeout=300) as resp:
                        if resp.status not in (200, 201):
                            raise RuntimeError(f"HTTP {resp.status}")
                    body.finish()
                    return
                except (urllib.error.URLError, OSError, RuntimeError) as e:
                    body.finish()
                    why = f"HTTP {e.code} {e.reason}" if isinstance(e, urllib.error.HTTPError) else str(e)
                    if attempt == RETRIES:
                        raise RuntimeError(f"PUT {key}: {why}, after {attempt} attempts") from None
                    wait = 5 * attempt
                    print(f"  retry {attempt}/{RETRIES} for {key} in {wait}s: {why}")
                    time.sleep(wait)


def public_head(url: str) -> dict[str, str] | None:
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "Moonshine-Publish/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return {k.lower(): v for k, v in resp.headers.items()}
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise RuntimeError(f"HEAD {url}: HTTP {e.code} {e.reason}") from None


def public_get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "Moonshine-Publish/1.0", "Cache-Control": "no-cache"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def credentials(from_stdin: bool) -> tuple[str, str, str]:
    if from_stdin:
        lines = [line.strip() for line in sys.stdin.read().splitlines() if line.strip()]
        if len(lines) != 3:
            fail("--creds-stdin wants three lines: the S3 endpoint, the access key id, the secret")
        return lines[0], lines[1], lines[2]
    env = os.environ
    endpoint = env.get("S3_ENDPOINT") or (
        f"https://{env['CLOUDFLARE_ACCOUNT_ID']}.r2.cloudflarestorage.com" if env.get("CLOUDFLARE_ACCOUNT_ID") else "")
    ak = env.get("AWS_ACCESS_KEY_ID") or env.get("CLOUDFLARE_ACCESS_KEY_ID") or ""
    sk = env.get("AWS_SECRET_ACCESS_KEY") or env.get("CLOUDFLARE_SECRET_ACCESS_KEY") or ""
    if not (endpoint and ak and sk):
        fail("credentials: S3_ENDPOINT (or CLOUDFLARE_ACCOUNT_ID), AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY "
             "(or the CLOUDFLARE_* pair) in the environment, or --creds-stdin")
    return endpoint, ak, sk


def key_of(url: str, base: str, prefix: str) -> str:
    """The bucket key a URL under --download-base names, each path segment unescaped."""
    if not url.startswith(base + "/"):
        raise ValueError(f"{url} is not under {base}")
    rel = "/".join(urllib.parse.unquote(seg) for seg in url[len(base) + 1:].split("/"))
    return f"{prefix}/{rel}" if prefix else rel


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="the release staged under release/publish/<version>")
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."),
                    help="the checkout Stage.ps1 ran in (default: this file's parent)")
    ap.add_argument("--download-base", default=os.environ.get("MOONSHINE_DOWNLOAD_BASE") or "https://download.moonshine.ai/kcd2",
                    help="where the manifest's URLs point (default: MOONSHINE_DOWNLOAD_BASE, or download.moonshine.ai/kcd2)")
    ap.add_argument("--bucket", default=os.environ.get("S3_BUCKET") or "download-moonshine-ai",
                    help="the bucket behind --download-base (default: S3_BUCKET, or download-moonshine-ai)")
    ap.add_argument("--creds-stdin", action="store_true", help="endpoint, access key, secret: three lines on stdin")
    ap.add_argument("--check", action="store_true", help="hash and HEAD, report, upload nothing")
    ap.add_argument("--no-latest", action="store_true", help="leave latest/manifest.json as it is (a release not yet announced)")
    args = ap.parse_args()

    version = args.version
    repo = os.path.abspath(args.repo)
    base = args.download_base.rstrip("/")
    prefix = urllib.parse.urlsplit(base).path.strip("/")
    stage = os.path.join(repo, "release", "stage", "Moonshine")
    publish = os.path.join(repo, "release", "publish", version)
    manifest_path = os.path.join(publish, "manifest.json")
    if not os.path.isfile(manifest_path):
        fail(f"no {manifest_path}; installer\\Stage.ps1 -Version {version} writes it")
    with open(manifest_path, "rb") as f:
        manifest_bytes = f.read()
    manifest = json.loads(manifest_bytes.decode("utf-8"))
    if manifest.get("version") != version:
        fail(f"{manifest_path} is for {manifest.get('version')}, not {version}")
    setup = manifest.get("setup") or {}
    if not setup.get("sha256"):
        fail("the manifest has no installer hash: Stage.ps1 was run with -SkipInstaller, or ISCC failed")
    if manifest.get("latest") != f"{base}/latest/manifest.json":
        fail(f"the manifest's `latest` is {manifest.get('latest')}; this is publishing under {base}")

    endpoint, ak, sk = credentials(args.creds_stdin)
    store = Store(endpoint, args.bucket, ak, sk)

    # --- what the manifest names, checked against the stage --------------------------------
    print(f"=== Moonshine {version}: checking release\\stage against {os.path.relpath(manifest_path, repo)}")
    hosted = [e for e in manifest["files"] if e["url"].startswith(base + "/")]
    plan: list[tuple[str, str, int, str]] = []  # (local path, key, bytes, cache-control)
    bad = 0
    for e in hosted:
        local = os.path.join(stage, *e["path"].split("/"))
        if not os.path.isfile(local):
            print(f"  missing  {e['path']}")
            bad += 1
            continue
        size = os.path.getsize(local)
        if size != e["bytes"] or digest(local, "sha256") != e["sha256"]:
            print(f"  WRONG    {e['path']} (not the bytes the manifest names)")
            bad += 1
            continue
        plan.append((local, key_of(e["url"], base, prefix), size, IMMUTABLE))
    setup_name = os.path.basename(urllib.parse.urlsplit(setup["url"]).path)
    setup_local = os.path.join(publish, setup_name)
    if not os.path.isfile(setup_local):
        print(f"  missing  {setup_name}")
        bad += 1
    elif os.path.getsize(setup_local) != setup["bytes"] or digest(setup_local, "sha256") != setup["sha256"]:
        print(f"  WRONG    {setup_name} (not the installer the manifest names)")
        bad += 1
    if bad:
        fail(f"  {bad} file(s) missing or wrong; stage again")
    print(f"  {len(hosted)} hosted file(s) and the installer match the manifest; "
          f"{len(manifest['files']) - len(hosted)} more come from their own pins (models)")

    # --- the blobs ---------------------------------------------------------------------------
    print(f"=== blobs under s3://{args.bucket}/{prefix}/blobs/")
    to_upload: list[tuple[str, str, int, str]] = []
    reused = 0
    for local, key, size, cc in plan:
        have = store.head(key)
        name = os.path.relpath(local, stage).replace(os.sep, "/")
        if have and int(have.get("content-length", -1)) == size:
            reused += size
            print(f"  have     {name}  ({mb(size)})")
        else:
            to_upload.append((local, key, size, cc))
            print(f"  {'upload' if not args.check else 'would':8} {name}  ({mb(size)})")
    print(f"  {len(plan) - len(to_upload)} already there ({mb(reused)}), {len(to_upload)} to upload ({mb(sum(s for _, _, s, _ in to_upload))})")

    # --- the per-version objects ---------------------------------------------------------------
    print(f"=== s3://{args.bucket}/{prefix}/{version}/")
    versioned = [
        (setup_local, key_of(setup["url"], base, prefix)),
        (manifest_path, f"{prefix}/{version}/manifest.json" if prefix else f"{version}/manifest.json"),
    ]
    for local, key in versioned:
        have = store.head(key)
        etag = (have or {}).get("etag", "").strip('"')
        same = have is not None and etag == digest(local, "md5")
        print(f"  {'have' if same else ('upload' if not args.check else 'would'):8} {key}" + (" (differs)" if have and not same else ""))
        if not same:
            to_upload.append((local, key, os.path.getsize(local), IMMUTABLE))
    latest_key = f"{prefix}/latest/manifest.json" if prefix else "latest/manifest.json"

    if args.check:
        print(f"check: {len(to_upload)} object(s) would be uploaded" + ("" if args.no_latest else f", then {latest_key}"))
        return 0

    # --- up ----------------------------------------------------------------------------------
    print("=== uploading")
    for local, key, size, cc in to_upload:
        ctype = "application/json" if key.endswith("manifest.json") else None
        print(f"  {mb(size):>12}  {key}")
        store.put(local, key, cc, ctype)

    # --- reachable, through the front door -----------------------------------------------------
    print(f"=== checking {base}/{version}/")
    got = public_get(f"{base}/{version}/manifest.json")
    if got != manifest_bytes:
        fail("  the manifest read back differs from the one uploaded")
    print(f"  manifest.json  ({len(got)} bytes, as uploaded)")
    h = public_head(setup["url"])
    if not h or int(h.get("content-length", -1)) != setup["bytes"]:
        fail(f"  {setup['url']} is {'missing' if not h else 'the wrong size'}")
    print(f"  {setup_name}  ({mb(setup['bytes'])})")
    wrong = 0
    for e in hosted:
        h = public_head(e["url"])
        if not h or int(h.get("content-length", -1)) != e["bytes"]:
            print(f"  NOT THERE  {e['url']}")
            wrong += 1
    if wrong:
        fail(f"  {wrong} blob(s) not reachable; latest/manifest.json left as it was")
    print(f"  {len(hosted)} blob(s) reachable")

    if args.no_latest:
        print(f"not touching {base}/latest/manifest.json (--no-latest)")
    else:
        print(f"=== {latest_key}")
        store.put(manifest_path, latest_key, SHORT, "application/json")
        latest = json.loads(public_get(f"{base}/latest/manifest.json").decode("utf-8"))
        if latest.get("version") != version:
            fail(f"  latest reads back as {latest.get('version')}; the edge may be serving the old one for a few minutes")
        print(f"  latest -> {version}")
    print(f"installer: {setup['url']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
