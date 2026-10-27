#!/usr/bin/env python3
"""Build the pak that lets the game play the mod's own mouth-shape clips.

The mod ships a short facial clip per mouth shape (Animations/humans/facials/moonshine/
mn_vis_*.caf, from scripts/facial_corpus.py bank). The game's animation system only knows
a clip through its *animation image*: with ca_UseIMG_CAF on (as shipped) every clip's
header -- path, length, controller count -- comes from Animations/Animations.img and
Animations/FacialAnimations.img, and the wildcards in the skeletons' chrparams
(`#filepath Animations\\humans\\facials` then `*\\*.caf`) are expanded against those headers,
not the paks. Found on the Windows laptop, 13 Oct 2026: the clips alone in the data pak are not found
(GetAnimationLength 0); a clip named outright in male.chrparams whose header no image
holds crashes the game as the level loads, a shipped loose clip named the same way too;
the host's Animations.img with a header per clip appended, and nothing else, has the clips
load (length 0.47 s) and play. So that is what ships: the image with our headers added as
it has its own loose clips (no tracks database, flags 0x2000, path CRC left for the game,
the length and controller count read off the clip). --chrparams adds the host's male and
female chrparams with a line per clip at the head of the animation list, which the game
did not need; it is kept for a build whose facial wildcard reads only the facial image.

The base files are the host's (a patch changes them): as with the scheduler override
(scripts/build_scheduler.py) they are read out of the game's own paks on the machine that
has it, so what ships is an edit of what that machine's game has. scripts/build_mod.sh
runs this with --game when it can. Everything it uses is stdlib; the clip headers are read
with scripts/kcd2_facial.py's reader.

    scripts/build_mouthpak.py --clips mod/assets/pak/Animations/humans/facials/moonshine \\
        --game '/path/to/KingdomComeDeliverance2' --out out/Data/moonshine_mouth.pak [--chrparams]

No clips in the folder writes no pak and says so.
"""

from __future__ import annotations

import argparse
import glob
import re
import struct
import sys
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kcd2_facial  # noqa: E402

# The skeletons every human NPC and Henry hang off.
CHRPARAMS = (
    "Objects/characters/humans/male/skeleton/male.chrparams",
    "Objects/characters/humans/female/skeleton/female.chrparams",
)
IMG = "Animations/Animations.img"
IMG_PAK = "Animations.pak"
CLIP_DIR = "Animations/humans/facials/moonshine"
HEADER_VERSION = 0x0971
HEADER_SIZE = 404
LOOSE_FLAGS = 0x2000


def read_member(pak: Path, info: zipfile.ZipInfo) -> bytes:
    """Read one pak member. CryEngine paks store a raw deflate stream (extract_kcd2_places.py)."""
    with open(pak, "rb") as f:
        f.seek(info.header_offset)
        _sig, _ver, _flag, method, _mt, _md, _crc, comp, _unc, namelen, extralen = struct.unpack(
            "<IHHHHHIIIHH", f.read(30))
        f.read(namelen + extralen)
        data = f.read(comp)
    return zlib.decompress(data, -15) if method == 8 else data


def read_from_paks(paks: list[str], wanted: tuple[str, ...]) -> dict[str, bytes]:
    """The wanted members, by their path as given, out of the first pak that has each."""
    want = {p.lower(): p for p in wanted}
    found: dict[str, bytes] = {}
    for pak in paks:
        try:
            z = zipfile.ZipFile(pak)
        except zipfile.BadZipFile:
            continue
        for info in z.infolist():
            key = info.filename.replace("\\", "/").lower()
            if key in want and want[key] not in found:
                found[want[key]] = read_member(Path(pak), info)
    missing = [p for p in wanted if p not in found]
    if missing:
        raise SystemExit("not in the game's paks: " + ", ".join(missing))
    return found


# ------------------------------------------------------------------ the animation image


def clip_facts(caf: bytes) -> tuple[float, int]:
    """(seconds, controller count) of a clip, from its motion parameters and controller chunks."""
    seconds = 0.0
    controllers = 0
    for ctype, _cver, _cid, body in kcd2_facial.chunks(caf):
        if ctype == kcd2_facial.CHUNK_MOTION_PARAMS:
            _flags, _comp, ticks, secs_per_tick, start, end = struct.unpack_from("<IIifii", body, 0)
            seconds = (end - start) * ticks * secs_per_tick
        elif ctype == kcd2_facial.CHUNK_CONTROLLER:
            controllers += 1
    return seconds, controllers


def clip_header(path: str, seconds: float, controllers: int) -> bytes:
    """A header as the image has them for its loose clips: flags, the path (256 bytes), the
    path's and the tracks database's CRCs (none: the game fills the first, the clip is its
    own file), eight heel and toe plants at -10000, start, end and duration in seconds,
    the controller count, start and end locations (identity), and 32 bytes of zeros."""
    p = path.lower().replace("\\", "/").encode("ascii")
    if len(p) > 255:
        raise SystemExit("clip path too long for a header: " + path)
    b = struct.pack("<I", LOOSE_FLAGS) + p + b"\0" * (256 - len(p))
    b += struct.pack("<II", 0, 0)
    b += struct.pack("<8f", *([-10000.0] * 8))
    b += struct.pack("<fffI", 0.0, seconds, seconds, controllers)
    b += struct.pack("<7f", 0, 0, 0, 1, 0, 0, 0) * 2
    b += b"\0" * 32
    assert len(b) == HEADER_SIZE
    return b


def with_headers(img: bytes, clips: dict[str, bytes]) -> bytes:
    """The image with a header per clip appended ({path in the pak: the clip's bytes})."""
    sig, version, count, table = struct.unpack_from("<4sIII", img, 0)
    if sig != b"CrCh":
        raise SystemExit("not a chunk file: %s" % IMG)
    rows = [struct.unpack_from("<HHIII", img, table + 16 * k) for k in range(count)]
    bodies = [img[off:off + size] for (_t, _v, _i, size, off) in rows]
    kinds = [(t, v) for (t, v, _i, _s, _o) in rows]
    next_id = max(i for (_t, _v, i, _s, _o) in rows) + 1
    for path in sorted(clips):
        seconds, controllers = clip_facts(clips[path])
        kinds.append((kcd2_facial.CHUNK_CAF_HEADER, HEADER_VERSION))
        bodies.append(clip_header(path, seconds, controllers))
    out_table = b""
    data_at = 16 + 16 * len(bodies)
    offset = data_at
    for k, ((t, v), body) in enumerate(zip(kinds, bodies)):
        cid = rows[k][2] if k < count else next_id + (k - count)
        out_table += struct.pack("<HHIII", t, v, cid, len(body), offset)
        offset += len(body)
    return struct.pack("<4sIII", b"CrCh", version, len(bodies), 16) + out_table + b"".join(bodies)


# ------------------------------------------------------------------ the chrparams


def with_clips(chrparams: bytes, stems: list[str]) -> bytes:
    """The chrparams with a line per clip added at the head of its AnimationList, before any
    #filepath (an explicit path after one would be taken relative to it)."""
    text = chrparams.decode("utf-8")
    m = re.search(r"<AnimationList>[^\n]*\n", text)
    if m is None:
        raise SystemExit("no <AnimationList> in the chrparams")
    indent = "\t\t"
    lines = "".join('%s<Animation name="%s" path="%s\\%s.caf"/>\n'
                    % (indent, stem, CLIP_DIR.replace("/", "\\"), stem) for stem in stems)
    head = ("%s<!-- The Moonshine mod's mouth shapes (scripts/build_mouthpak.py); the rest is the game's own. -->\n"
            % indent)
    return (text[:m.end()] + head + lines + text[m.end():]).encode("utf-8")


# ------------------------------------------------------------------ the pak


def write_pak(path: Path, members: dict[str, bytes]) -> None:
    """Deflated entries at the DOS epoch with no extra fields (a pak written by Info-ZIP `zip`
    carries fields the game chokes on: behaviors.md §4.6); a fixed date so unchanged
    content is an unchanged pak and the deploy copies nothing."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        for name in sorted(members):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 0
            z.writestr(info, members[name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def build(game: Path, clips: dict[str, bytes], out: Path, chrparams: bool) -> None:
    members: dict[str, bytes] = {}
    img = read_from_paks([str(game / "Data" / IMG_PAK)], (IMG,))[IMG]
    members[IMG] = with_headers(img, clips)
    if chrparams:
        base = read_from_paks(sorted(glob.glob(str(game / "Data" / "IPL_Characters*.pak"))), CHRPARAMS)
        stems = sorted(Path(p).stem for p in clips)
        members.update({p: with_clips(b, stems) for p, b in base.items()})
    write_pak(out, members)
    print("  %s: %d clips' headers in %s%s" % (out, len(clips), IMG,
          ", and the clips named in %d skeletons' chrparams" % len(CHRPARAMS) if chrparams else ""), file=sys.stderr)


def read_clips(folder: Path) -> dict[str, bytes]:
    return {"%s/%s" % (CLIP_DIR, Path(p).name): Path(p).read_bytes() for p in glob.glob(str(folder / "*.caf"))}


# ------------------------------------------------------------------ main


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clips", type=Path, required=True, help="the folder of the mod's own clips (mn_vis_*.caf)")
    ap.add_argument("--game", help="the game root, to read the image and chrparams out of its paks")
    ap.add_argument("--out", type=Path, help="the pak to write (Data/moonshine_mouth.pak in the mod)")
    ap.add_argument("--chrparams", action="store_true", help="also name the clips in the skeletons' chrparams")
    args = ap.parse_args()

    if not glob.glob(str(args.clips / "*.caf")):
        print("  no mouth-shape clips; no animation override", file=sys.stderr)
        return
    if not args.out:
        raise SystemExit("--out is needed to build")
    if not args.game:
        raise SystemExit("--game is needed for the base files")
    build(Path(args.game).expanduser(), read_clips(args.clips), args.out, args.chrparams)


if __name__ == "__main__":
    main()
