#!/usr/bin/env python3
"""Build npcs.sqlite, and npcs-vectors.sqlite beside it, out of a data drop, and lay the
drop's voices where the app reads them.

A drop (scripts/export_drop.py in moonshine-voice-characters) is the shipped tables one
file each -- TSV for the flat ones, JSONL for the prose, one float32 matrix for the
knowledge vectors -- under a manifest of sha256s. This runs data/schema.sql into two fresh
files (the schema says where it divides) and loads them, in the drop's own (sorted) order,
so that the same drop gives the same bytes on any machine; the mod generator's paks come
out identical, which is how the pair is checked. Standard library only: it runs on the
Windows build host's Python as it is.

The vectors are not stored as the drop carries them. EmbeddingGemma's 768 float32s are
quantised to signed bytes with one scale per vector: 768 bytes a row where there were 3,072,
in a file of their own that changes only when the knowledge does, while the text beside it
changes with every drop. KEEP_DIMS says how many leading components are kept (the model is
trained so a prefix is an embedding in its own right, re-normalised; 768 is all of them,
and 256 was measured to cost one best item in ten); the app cuts its queries to whatever
dim the rows say.

    tools/assemble_db.py drop/2026-10-27-abc1234 data/kcd2/npcs.sqlite
                                                 ...and data/kcd2/npcs-vectors.sqlite
    tools/assemble_db.py DROP OUT --voices-out data/kcd2/voices     and the voices too
    tools/assemble_db.py DROP OUT --no-verify                       skip the manifest's hashes
    tools/assemble_db.py --compare a.sqlite b.sqlite                the shipped tables, row by row

--compare is what export_drop.py --check uses: the working database against the one built
from its drop, every table the schema names, every value with its type; the vectors from
the companion files when both sides have one. It says the first difference and exits 1.
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import shutil
import sqlite3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
VECTOR_TABLE = "npc_kb_vector"
# The line of data/schema.sql after which the statements are the vectors' file.
VECTORS_MARK = "-- ==== npcs-vectors.sqlite"
# How many of the embedding's leading components are kept: all of them (256 ranked worse).
KEEP_DIMS = 768
# 16 KB pages: the knowledge rows are a few kilobytes each, and 4 KB pages left most of a
# page empty beside every vector; it costs nothing on the reads the app does.
PAGE_SIZE = 16384


def split_schema(schema_sql: str) -> tuple[str, str]:
    """(npcs.sqlite's statements, npcs-vectors.sqlite's), divided at VECTORS_MARK."""
    at = schema_sql.find(VECTORS_MARK)
    if at < 0:
        raise SystemExit(f"schema.sql has no '{VECTORS_MARK}' line to divide the two files at")
    return schema_sql[:at], schema_sql[at:]


def vectors_path(db: Path) -> Path:
    """data/kcd2/npcs.sqlite -> data/kcd2/npcs-vectors.sqlite."""
    return db.with_name(db.stem + "-vectors" + db.suffix)


def quantize(raw: bytes, dim: int) -> tuple[int, float, bytes]:
    """One float32 row of the drop's matrix (little-endian, dim wide) as the stored form:
    (kept dim, scale, int8 bytes). The first KEEP_DIMS components, re-normalised, each
    divided by scale (the largest magnitude over 127) and rounded; round-half-even, like
    everything else here, so two machines agree to the byte."""
    keep = min(dim, KEEP_DIMS)
    a = array.array("f")
    a.frombytes(raw[:keep * 4])
    if sys.byteorder == "big":
        a.byteswap()
    norm = math.sqrt(math.fsum(x * x for x in a))
    if norm == 0.0:
        return keep, 1.0, bytes(keep)
    inv = 1.0 / norm
    largest = max(abs(x) for x in a) * inv
    scale = largest / 127.0
    k = inv / scale
    q = array.array("b", [max(-127, min(127, int(round(x * k)))) for x in a])
    return keep, scale, q.tobytes()


def dequantize(scale: float, vec: bytes) -> list[float]:
    """The floats a stored row stands for, re-normalised, as the app rebuilds them."""
    q = array.array("b")
    q.frombytes(vec)
    v = [x * scale for x in q]
    norm = math.sqrt(math.fsum(x * x for x in v))
    return [x / norm for x in v] if norm else v
VOICE_DIRS = ["clones", "phrases"]
VOICE_FILES = ["facials.txt", "lexicon.tsv"]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tsv_unescape(field: str):
    if field == r"\N":
        return None
    if "\\" not in field:
        return field
    out = []
    i = 0
    while i < len(field):
        c = field[i]
        if c == "\\" and i + 1 < len(field):
            n = field[i + 1]
            out.append({"\\": "\\", "t": "\t", "n": "\n", "r": "\r", "N": "\\N"}.get(n, "\\" + n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def schema_tables(schema_sql: str) -> list[tuple[str, list[str], list[str]]]:
    mem = sqlite3.connect(":memory:")
    mem.executescript(schema_sql)
    out = []
    for (name,) in mem.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY rowid"):
        info = mem.execute(f"PRAGMA table_info(\"{name}\")").fetchall()
        cols = [row[1] for row in info]
        pk = [row[1] for row in sorted((r for r in info if r[5] > 0), key=lambda r: r[5])]
        out.append((name, cols, pk or cols))
    mem.close()
    return out


def verify(drop: Path) -> dict:
    manifest = json.loads((drop / "manifest.json").read_text(encoding="utf-8"))
    bad = []
    for rel, want in manifest["files"].items():
        p = drop / rel
        if not p.exists():
            bad.append(f"missing {rel}")
        elif p.stat().st_size != want["bytes"] or sha256_of(p) != want["sha256"]:
            bad.append(f"differs {rel}")
    if bad:
        for b in bad[:20]:
            print("  " + b, file=sys.stderr)
        raise SystemExit(f"{len(bad)} files do not match {drop / 'manifest.json'}")
    return manifest


def load_table(db: sqlite3.Connection, drop_db: Path, table: str, cols: list[str]) -> int:
    tsv = drop_db / f"{table}.tsv"
    jsonl = drop_db / f"{table}.jsonl"
    placeholders = ", ".join("?" for _ in cols)
    names = ", ".join(f'"{c}"' for c in cols)
    sql = f'INSERT INTO "{table}" ({names}) VALUES ({placeholders})'
    n = 0
    if tsv.exists():
        with tsv.open("r", encoding="utf-8", newline="\n") as f:
            header = f.readline().rstrip("\n").split("\t")
            if header != cols:
                raise SystemExit(f"{tsv.name}: columns {header} are not the schema's {cols}")
            batch = []
            for line in f:
                fields = line.rstrip("\n").split("\t")
                if len(fields) != len(cols):
                    raise SystemExit(f"{tsv.name}: {len(fields)} fields for {len(cols)} columns")
                batch.append([tsv_unescape(x) for x in fields])
                if len(batch) >= 5000:
                    db.executemany(sql, batch)
                    n += len(batch)
                    batch = []
            if batch:
                db.executemany(sql, batch)
                n += len(batch)
    elif jsonl.exists():
        with jsonl.open("r", encoding="utf-8") as f:
            batch = []
            for line in f:
                if not line.strip():
                    continue
                obj = json.loads(line)
                if list(obj.keys()) != cols:
                    raise SystemExit(f"{jsonl.name}: keys {list(obj)} are not the schema's {cols}")
                batch.append([obj[c] for c in cols])
                if len(batch) >= 5000:
                    db.executemany(sql, batch)
                    n += len(batch)
                    batch = []
            if batch:
                db.executemany(sql, batch)
                n += len(batch)
    else:
        print(f"  {table}: not in the drop; left empty", file=sys.stderr)
    return n


def load_vectors(db: sqlite3.Connection, drop_db: Path) -> int:
    matrix = drop_db / f"{VECTOR_TABLE}.f32"
    index = drop_db / f"{VECTOR_TABLE}.index.tsv"
    if not matrix.exists() or not index.exists():
        print(f"  {VECTOR_TABLE}: not in the drop; left empty", file=sys.stderr)
        return 0
    sql = f'INSERT INTO "{VECTOR_TABLE}" (sha, dim, scale, vec) VALUES (?, ?, ?, ?)'
    n = 0
    with matrix.open("rb") as mf, index.open("r", encoding="utf-8") as xf:
        header = xf.readline().rstrip("\n").split("\t")
        if header != ["sha", "dim", "row"]:
            raise SystemExit(f"{index.name}: unexpected header {header}")
        batch = []
        expect_row = 0
        for line in xf:
            sha, dim_s, row_s = line.rstrip("\n").split("\t")
            dim, row = int(dim_s), int(row_s)
            if row != expect_row:
                raise SystemExit(f"{index.name}: row {row} where {expect_row} was expected")
            expect_row += 1
            raw = mf.read(dim * 4)
            if len(raw) != dim * 4:
                raise SystemExit(f"{matrix.name}: short read at row {row}")
            kept, scale, q = quantize(raw, dim)
            batch.append((bytes.fromhex(sha), kept, scale, q))
            if len(batch) >= 2000:
                db.executemany(sql, batch)
                n += len(batch)
                batch = []
        if batch:
            db.executemany(sql, batch)
            n += len(batch)
        if mf.read(1):
            raise SystemExit(f"{matrix.name}: bytes beyond the index's last row")
    return n


def build(out: Path, schema_sql: str, load) -> None:
    """One file: the schema run into a fresh database, `load(db, table, cols)` for each of
    its tables, VACUUM, integrity check. Written beside the destination and moved into
    place at the end, so a half-built file is never what the app opens; a running app on
    Windows holds the old one, in which case the move says so."""
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".building")
    tmp.unlink(missing_ok=True)
    db = sqlite3.connect(tmp)
    db.execute("PRAGMA journal_mode = OFF")
    db.execute("PRAGMA synchronous = OFF")
    db.execute(f"PRAGMA page_size = {PAGE_SIZE}")
    db.executescript(schema_sql)
    db.execute("BEGIN")
    for table, cols, _pk in schema_tables(schema_sql):
        n = load(db, table, cols)
        print(f"  {table}: {n} rows")
    db.commit()
    db.execute("VACUUM")
    ok = db.execute("PRAGMA integrity_check").fetchone()[0]
    db.close()
    if ok != "ok":
        raise SystemExit(f"integrity_check: {ok}")
    os.replace(tmp, out)
    print(f"=== {out}: {out.stat().st_size >> 20} MB ===")


def assemble(drop: Path, out: Path, schema_sql: str, do_verify: bool) -> None:
    if not (drop / "manifest.json").exists():
        raise SystemExit(f"{drop} has no manifest.json; is it a drop?")
    if do_verify:
        print("=== verifying the manifest ===")
        manifest = verify(drop)
    else:
        manifest = json.loads((drop / "manifest.json").read_text(encoding="utf-8"))
    schema_sha = hashlib.sha256(schema_sql.encode("utf-8")).hexdigest()
    if manifest.get("schema_sha256") != schema_sha:
        print("  note: the drop was cut against another revision of schema.sql; columns the "
              "schema has and the drop lacks are NULL, columns the drop has and the schema "
              "lacks are an error", file=sys.stderr)
    main_sql, vectors_sql = split_schema(schema_sql)
    print(f"=== loading {drop.name} ===")
    build(out, main_sql, lambda db, table, cols: load_table(db, drop / "db", table, cols))
    build(vectors_path(out), vectors_sql,
          lambda db, table, cols: load_vectors(db, drop / "db") if table == VECTOR_TABLE
          else load_table(db, drop / "db", table, cols))


def place_voices(drop: Path, voices_out: Path) -> None:
    src = drop / "voices"
    if not src.is_dir():
        print(f"  the drop has no voices/", file=sys.stderr)
        return
    print(f"=== voices into {voices_out} ===")
    voices_out.mkdir(parents=True, exist_ok=True)
    for d in VOICE_DIRS:
        if not (src / d).is_dir():
            continue
        target = voices_out / d
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(src / d, target)
        print(f"  {d}/: {sum(1 for _ in target.rglob('*') if _.is_file())} files")
    for name in VOICE_FILES:
        if (src / name).exists():
            shutil.copy2(src / name, voices_out / name)
            print(f"  {name}")


def compare_tables(a: sqlite3.Connection, b: sqlite3.Connection, schema_sql: str) -> int:
    rc = 0
    for table, cols, pk in schema_tables(schema_sql):
        select = ", ".join(f'typeof("{c}"), "{c}"' for c in cols)
        order = ", ".join(f'"{c}"' for c in pk)
        sql = f'SELECT {select} FROM "{table}" ORDER BY {order}'
        try:
            ra, rb = a.execute(sql), b.execute(sql)
        except sqlite3.OperationalError as e:
            print(f"  {table}: {e}")
            rc = 1
            continue
        n = 0
        same = True
        while True:
            x, y = ra.fetchone(), rb.fetchone()
            if x is None and y is None:
                break
            if x != y:
                key = dict(zip(cols, (x or y)[1::2]))
                print(f"  {table}: row {n} differs ({', '.join(f'{k}={key[k]!r}' for k in pk)})")
                for i, c in enumerate(cols):
                    xa = x[2 * i:2 * i + 2] if x else None
                    ya = y[2 * i:2 * i + 2] if y else None
                    if xa != ya:
                        print(f"    {c}: {xa!r} vs {ya!r}")
                        break
                same = False
                rc = 1
                break
            n += 1
        print(f"  {table}: {'same' if same else 'DIFFERENT'} ({n} rows)")
    return rc


def compare(a_path: Path, b_path: Path, schema_sql: str) -> int:
    main_sql, vectors_sql = split_schema(schema_sql)
    a = sqlite3.connect(f"file:{a_path}?mode=ro", uri=True)
    b = sqlite3.connect(f"file:{b_path}?mode=ro", uri=True)
    rc = compare_tables(a, b, main_sql)
    a.close()
    b.close()
    va, vb = vectors_path(a_path), vectors_path(b_path)
    if va.exists() and vb.exists():
        a = sqlite3.connect(f"file:{va}?mode=ro", uri=True)
        b = sqlite3.connect(f"file:{vb}?mode=ro", uri=True)
        rc = compare_tables(a, b, vectors_sql) or rc
        a.close()
        b.close()
    else:
        print(f"  {VECTOR_TABLE}: not compared ({va.name} beside one side or both is missing)")
    print("same" if rc == 0 else "different")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("drop", nargs="?", help="the drop directory")
    ap.add_argument("out", nargs="?", help="the npcs.sqlite to write (npcs-vectors.sqlite goes beside it)")
    ap.add_argument("--schema", default=str(HERE / "data" / "schema.sql"))
    ap.add_argument("--voices-out", help="copy the drop's voices here (data/kcd2/voices)")
    ap.add_argument("--no-verify", action="store_true", help="do not check the manifest's hashes")
    ap.add_argument("--compare", nargs=2, metavar=("A.sqlite", "B.sqlite"),
                    help="compare the shipped tables of two databases and exit")
    args = ap.parse_args()

    schema_path = Path(args.schema)
    if not schema_path.exists():
        print(f"no schema at {schema_path}", file=sys.stderr)
        return 1
    schema_sql = schema_path.read_text(encoding="utf-8")

    if args.compare:
        return compare(Path(args.compare[0]), Path(args.compare[1]), schema_sql)
    if not args.drop or not args.out:
        ap.error("DROP and OUT are needed (or --compare)")
    drop = Path(args.drop)
    assemble(drop, Path(args.out), schema_sql, not args.no_verify)
    if args.voices_out:
        place_voices(drop, Path(args.voices_out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
