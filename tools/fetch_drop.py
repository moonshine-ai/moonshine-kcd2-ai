#!/usr/bin/env python3
"""Bring the pinned data drop into drop/ and assemble it into data/kcd2/.

data/DROP names the drop and the sha256 of its manifest. This fetches that drop from
wherever drops are kept -- a directory on this machine or a mounted share, an s3:// bucket
(any S3-compatible store, signed here with the standard library), a gs:// bucket (through
gcloud), or an https:// prefix -- checks every file against the manifest, and
then runs tools/assemble_db.py so that data/kcd2/npcs.sqlite, npcs-vectors.sqlite and voices/ are what
the app, the mod generator and the installer read. Files already present at the right size
and hash are not fetched again, so a new drop that changes one table moves one file.

    tools/fetch_drop.py --from /Volumes/drops            a directory holding <drop>/
    tools/fetch_drop.py --from s3://my-bucket/drops      S3-compatible (R2, S3, B2, MinIO): the
                                                          endpoint and keys from S3_ENDPOINT,
                                                          AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY
    tools/fetch_drop.py --from gs://my-bucket/drops      gcloud storage, for a private bucket
    tools/fetch_drop.py --from https://host/drops        plain https
    tools/fetch_drop.py --assemble                        drop/ is in place; only assemble
    tools/fetch_drop.py --check                           say what is missing; fetch nothing

--from defaults to $MOONSHINE_DROP_BASE. There is no built-in location: where a team keeps
its drops is that team's business, not this repository's. Standard library only, like
assemble_db.py, so it runs on the Windows build host's Python as it is.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "tools"))
import assemble_db  # noqa: E402
import s3_sign  # noqa: E402


def read_pin(path: Path) -> tuple[str, str]:
    pin = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            pin[k.strip()] = v.strip()
    if "drop" not in pin or "manifest_sha256" not in pin:
        sys.exit(f"{path}: wants drop= and manifest_sha256= lines")
    return pin["drop"], pin["manifest_sha256"]


class Source:
    """One drop at one base, reachable by copy, S3 signature, gcloud or https."""

    def __init__(self, base: str, drop: str):
        self.base = base.rstrip("/")
        self.drop = drop
        if base.startswith("s3://"):
            self.kind = "s3"
            self.endpoint, self.access_key, self.secret_key = s3_sign.from_env()
            self.bucket, self.prefix = s3_sign.split_s3(self.base)
        elif base.startswith("gs://"):
            self.kind = "gs"
        elif base.startswith(("http://", "https://")):
            self.kind = "http"
        else:
            self.kind = "dir"
            root = Path(base).expanduser()
            # Either the directory that holds <drop>/, or the drop itself.
            if (root / "manifest.json").exists() and root.name == drop:
                root = root.parent
            self.base = str(root)
            if not (root / drop / "manifest.json").exists():
                sys.exit(f"no {drop}/manifest.json under {root}")

    def fetch(self, rel: str, to: Path) -> None:
        to.parent.mkdir(parents=True, exist_ok=True)
        part = to.with_name(to.name + ".part")
        if self.kind == "dir":
            shutil.copyfile(Path(self.base) / self.drop / rel, part)
        elif self.kind == "s3":
            key = "/".join(x for x in (self.prefix, self.drop, rel) if x)
            url = s3_sign.presign("GET", self.endpoint, self.bucket, key, self.access_key, self.secret_key, 600)
            self._http(url, part)
        elif self.kind == "gs":
            url = f"{self.base}/{self.drop}/{rel}"
            rc = subprocess.run(["gcloud", "storage", "cp", "--no-user-output-enabled", url, str(part)]).returncode
            if rc:
                sys.exit(f"gcloud storage cp {url} failed ({rc})")
        else:
            self._http(f"{self.base}/{self.drop}/{urllib.request.quote(rel)}", part)
        os.replace(part, to)

    @staticmethod
    def _http(url: str, part: Path) -> None:
        # Resumed rather than restarted on a dropped connection: a 1.6 GB file over a
        # laptop's wifi does not always arrive in one go.
        for attempt in range(5):
            have = part.stat().st_size if part.exists() else 0
            req = urllib.request.Request(url, headers={"Range": f"bytes={have}-"} if have else {})
            try:
                with urllib.request.urlopen(req, timeout=120) as r, part.open("ab" if have else "wb") as f:
                    shutil.copyfileobj(r, f, 1 << 20)
                return
            except urllib.error.HTTPError as e:
                if e.code == 416 and have:  # already whole
                    return
                if e.code == 403 and attempt == 0:
                    sys.exit(f"{e.code} from {url.split('?')[0]}: the key cannot read it, or the clock here is off")
                if e.code < 500:
                    raise
            except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
                if attempt == 4:
                    raise
                print(f"    retrying after: {e}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="base", default=os.environ.get("MOONSHINE_DROP_BASE"),
                    help="directory, s3://, gs:// or https:// prefix holding <drop>/ (default $MOONSHINE_DROP_BASE)")
    ap.add_argument("--pin", default=str(HERE / "data" / "DROP"), help="the data/DROP file")
    ap.add_argument("--out", default=str(HERE / "drop"), help="where drops are kept locally")
    ap.add_argument("--db", default=str(HERE / "data" / "kcd2" / "npcs.sqlite"))
    ap.add_argument("--voices", default=str(HERE / "data" / "kcd2" / "voices"))
    ap.add_argument("--assemble", action="store_true", help="fetch nothing; assemble what drop/ has")
    ap.add_argument("--check", action="store_true", help="fetch nothing; report what is missing")
    ap.add_argument("--no-assemble", action="store_true", help="fetch only")
    args = ap.parse_args()

    drop, want_manifest = read_pin(Path(args.pin))
    local = Path(args.out) / drop
    manifest_path = local / "manifest.json"
    source = None
    if not (args.assemble or args.check):
        if not args.base:
            sys.exit("where are the drops? --from DIR|s3://...|gs://...|https://..., or MOONSHINE_DROP_BASE")
        source = Source(args.base, drop)

    # The manifest first, and it must be the pinned one: everything after trusts it.
    if not manifest_path.exists() or assemble_db.sha256_of(manifest_path) != want_manifest:
        if args.check:
            print(f"missing or stale: {drop}/manifest.json")
            return 1
        if args.assemble:
            sys.exit(f"{manifest_path} is missing or not the pinned manifest; fetch first")
        print(f"=== {drop}: manifest ===")
        source.fetch("manifest.json", manifest_path)
        have = assemble_db.sha256_of(manifest_path)
        if have != want_manifest:
            manifest_path.unlink()
            sys.exit(f"{drop}/manifest.json at {args.base} has sha256 {have}; data/DROP pins {want_manifest}")
    manifest = json.loads(manifest_path.read_text())

    if not args.assemble:
        missing = []
        for rel, meta in sorted(manifest["files"].items()):
            p = local / rel
            if p.exists() and p.stat().st_size == meta["bytes"] and assemble_db.sha256_of(p) == meta["sha256"]:
                continue
            missing.append(rel)
        if args.check:
            for rel in missing:
                print(f"missing or wrong: {drop}/{rel}")
            print(f"{len(manifest['files']) - len(missing)} of {len(manifest['files'])} files in place under {local}")
            return 1 if missing else 0
        if missing:
            total = sum(manifest["files"][r]["bytes"] for r in missing)
            print(f"=== {drop}: fetching {len(missing)} files, {total / 1e6:.0f} MB ===")
            for i, rel in enumerate(missing, 1):
                print(f"  [{i}/{len(missing)}] {rel}")
                source.fetch(rel, local / rel)
                if assemble_db.sha256_of(local / rel) != manifest["files"][rel]["sha256"]:
                    sys.exit(f"{rel}: fetched bytes do not match the manifest")
        else:
            print(f"=== {drop}: all {len(manifest['files'])} files in place ===")
        # Files from an older layout of the same drop name, which the manifest no longer lists.
        for p in local.rglob("*"):
            if p.is_file() and p.name != "manifest.json" and p.relative_to(local).as_posix() not in manifest["files"]:
                print(f"  removing {p.relative_to(local)}")
                p.unlink()

    if args.no_assemble:
        return 0

    # Assemble once per drop and schema: a stamp beside the database says which manifest
    # built it, against which schema.sql, so a change to either builds it again.
    db = Path(args.db)
    vectors = assemble_db.vectors_path(db)
    stamp = db.with_name(db.name + ".from")
    schema = (HERE / "data" / "schema.sql").read_text()
    built_from = f"{drop} {want_manifest} {hashlib.sha256(schema.encode('utf-8')).hexdigest()}"
    if db.exists() and vectors.exists() and stamp.exists() and stamp.read_text().strip() == built_from:
        print(f"=== {db} and {vectors.name} are already assembled from {drop} ===")
    else:
        # The manifest was checked file by file above; assemble_db's own pass would be a
        # second read of 2 GB.
        assemble_db.assemble(local, db, schema, do_verify=False)
        stamp.write_text(built_from + "\n")
    assemble_db.place_voices(local, Path(args.voices))
    print(f"ready: {db}, {vectors}, {args.voices}, assets under {local / 'assets'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
