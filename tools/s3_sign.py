#!/usr/bin/env python3
"""Presigned URLs for an S3-compatible object store, standard library only.

Signature Version 4, query-string form: the URL carries the credential, the date, the
lifetime and the signature, so whoever holds it can make that one request until it
expires and nothing else. Works against Amazon S3 and the stores that speak its API
(Cloudflare R2, Backblaze B2, MinIO, ...); the endpoint says which.

    from s3_sign import presign
    url = presign("GET", "https://ACCOUNT.r2.cloudflarestorage.com", "bucket", "drops/x/manifest.json",
                  access_key, secret_key, expires=600)

    python3 tools/s3_sign.py GET s3://bucket/key [--expires 604800]    prints the URL, with the
        endpoint and keys from S3_ENDPOINT, AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY

Path-style addressing (https://endpoint/bucket/key), which every S3-compatible store
accepts; region "auto" unless S3_REGION says otherwise, which is what R2 wants and what
S3 itself ignores for presigned URLs on most endpoints. Seven days (604800 s) is the most
S3 allows a presigned URL to live.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import hmac
import os
import sys
import urllib.parse


def _hmac(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode(), hashlib.sha256).digest()


def presign(method: str, endpoint: str, bucket: str, key: str, access_key: str, secret_key: str,
            expires: int = 600, region: str | None = None, now: datetime.datetime | None = None) -> str:
    region = region or os.environ.get("S3_REGION") or "auto"
    now = now or datetime.datetime.now(datetime.timezone.utc)
    amz_date = now.strftime("%Y%m%dT%H%M%SZ")
    datestamp = now.strftime("%Y%m%d")
    host = urllib.parse.urlsplit(endpoint).netloc
    scheme = urllib.parse.urlsplit(endpoint).scheme or "https"
    scope = f"{datestamp}/{region}/s3/aws4_request"
    # Each path segment URI-encoded once; the slashes between them kept.
    canonical_uri = "/" + "/".join(urllib.parse.quote(seg, safe="~") for seg in f"{bucket}/{key}".split("/"))
    params = {
        "X-Amz-Algorithm": "AWS4-HMAC-SHA256",
        "X-Amz-Credential": f"{access_key}/{scope}",
        "X-Amz-Date": amz_date,
        "X-Amz-Expires": str(int(expires)),
        "X-Amz-SignedHeaders": "host",
    }
    canonical_query = "&".join(f"{urllib.parse.quote(k, safe='~')}={urllib.parse.quote(v, safe='~')}"
                               for k, v in sorted(params.items()))
    canonical_request = "\n".join([method.upper(), canonical_uri, canonical_query,
                                   f"host:{host}\n", "host", "UNSIGNED-PAYLOAD"])
    string_to_sign = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope,
                                hashlib.sha256(canonical_request.encode()).hexdigest()])
    k = _hmac(("AWS4" + secret_key).encode(), datestamp)
    k = _hmac(k, region)
    k = _hmac(k, "s3")
    k = _hmac(k, "aws4_request")
    signature = hmac.new(k, string_to_sign.encode(), hashlib.sha256).hexdigest()
    return f"{scheme}://{host}{canonical_uri}?{canonical_query}&X-Amz-Signature={signature}"


def split_s3(url: str) -> tuple[str, str]:
    """s3://bucket/some/key -> ("bucket", "some/key")."""
    if not url.startswith("s3://"):
        raise ValueError(f"not an s3:// url: {url}")
    bucket, _, key = url[5:].partition("/")
    return bucket, key


def from_env() -> tuple[str, str, str]:
    """(endpoint, access key, secret) from S3_ENDPOINT, AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY."""
    missing = [v for v in ("S3_ENDPOINT", "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY") if not os.environ.get(v)]
    if missing:
        sys.exit(f"s3:// needs {', '.join(missing)} in the environment")
    return os.environ["S3_ENDPOINT"], os.environ["AWS_ACCESS_KEY_ID"], os.environ["AWS_SECRET_ACCESS_KEY"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("method", choices=["GET", "PUT", "HEAD", "DELETE"])
    ap.add_argument("url", help="s3://bucket/key")
    ap.add_argument("--expires", type=int, default=604800, help="seconds (default 7 days, the most allowed)")
    args = ap.parse_args()
    endpoint, ak, sk = from_env()
    bucket, key = split_s3(args.url)
    print(presign(args.method, endpoint, bucket, key, ak, sk, args.expires))
    return 0


if __name__ == "__main__":
    sys.exit(main())
