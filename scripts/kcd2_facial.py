#!/usr/bin/env python3
"""The game's facial animation, read from its own files: what a face does, frame by frame.

KCD2 animates faces with a pose rig: 103 joints named FA_POSE_<side>_<pose> (FA_POSE_C_jawDrop,
FA_POSE_L_cornerPull, ...) hang off the skeleton's root, and each one's translation is the
weight of that pose on the face -- a FACS-like set: jawDrop, lipPart, funnel, O, U, kiss,
press, cornerPull, browsUp, eyelidClose and so on. A spoken line's facial clip is nothing
but those weights over time at 30 frames a second, so reading the clips gives the mouth
shapes behind 295,556 lines of dialogue, and writing one puts any mouth shapes we like
on a face (scripts/build_facial_index.py reads them; write_caf and pose_clip below write).

The clips live in Data/Facials/Facials_english-part*.pak, in CryEngine's DBA form: one chunk
file (CrCh 0x746) per scene with a single controller chunk, type 0x100d version 0x0905,
holding every clip of the scene with their tracks shared and deduplicated. The layout, as
Markemp's 010 templates have it and as checked against these files (13 Oct 2026):

    uint32 numKeyPos, numKeyRot, numKeyTime, numAnims
    uint16 keyTimeSizes[numKeyTime]      uint32 keyTimeFormatCounts[7]   -- tracks are sorted by format,
    uint16 positionSizes[numKeyPos]      uint32 positionFormatCounts[9]     so a count per format says
    uint16 rotationSizes[numKeyRot]      uint32 rotationFormatCounts[9]     which format each track has
    int32 keyTimeOffsets[numKeyTime], positionOffsets[numKeyPos], rotationOffsets[numKeyRot + 1]
    (offsets negative: "in-place streaming" -- uint32 padding length and that many bytes follow,
     offsets count back from the end of the chunk, and the track data is the chunk's tail)
    per animation: uint16 pathLength, path, MotionParams (132 bytes: flags, compression,
      ticksPerFrame, secsPerTick, start, end frames, ...), uint16 footplant bits (count, bytes),
      uint16 controllerCount, int32 offset from the first entry to this one's controllers
    all the controllers: uint32 id (CRC32 of the joint name as written), posKeyTimeTrack,
      posTrack, rotKeyTimeTrack, rotTrack (0xffffffff for none)
    the track data

A "size" is in the format's unit: keys for Byte/UINT16/F32 times and for positions
(12 bytes a key, three floats), uint16 words for a Bitset (start frame, end frame, key
count, then a bit per frame). Animations/FacialAnimations.img in part0 is the game's cache of every
clip's header (type 0x3007 version 0x0971: flags, path[256], path CRC32, DBA path CRC32,
heel/toe plants, start, end and duration in seconds, controller count, start and end
locations), which is how a clip is found: its header names the DBA that holds it.

Toward a mouth made from the words rather than borrowed (13 Oct 2026, what is known):
  - a clip's stem is the actor's code and the line's text id lowercased
    (tmck_jind_henry_musim_se_vrati_aixp <- jind_henry_musim_se_vrati_AIXP, "I have to help
    them."), and the text ids with their English are Localization/English_xml.pak's
    text_ui_dialog.xml, 177,930 rows of <Cell>id</Cell><Cell>text</Cell><Cell>text</Cell>: so
    295,556 clips pair with their words, a corpus for learning which pose weights which
    sounds make, in 86 actors' faces;
  - a clip is 30 frames a second of weights on about 50 of the 103 poses, 0 to 0.9, keyed
    only where they change (the Bitset key times) and interpolated between -- small and
    simple to write back;
  - male.chrparams takes any .caf under Animations/humans/facials by wildcard, but the
    game expands that wildcard against its animation image, not the paks: a clip of a
    mod's in the data pak is not found, naming it outright in the chrparams crashes the
    level load, and the host's Animations/Animations.img with a header per clip appended
    (scripts/build_mouthpak.py) has it load and play (the Windows laptop, 13 Oct 2026). The clips
    the mod ships are CAFs from pose_clip below (chunk 0x100d version 0x0829, positions
    only, uncompressed): scripts/facial_corpus.py bank writes one per mouth shape.

    python3 scripts/kcd2_facial.py bones male.chr                      the joint names and ids
    python3 scripts/kcd2_facial.py clips some.dba                       what a DBA holds
    python3 scripts/kcd2_facial.py dump some.dba <stem> [--chr male.chr]  a clip's weights by frame
    python3 scripts/kcd2_facial.py mouth some.dba <stem>                the mouth's openness, 100 ms steps

Run on the Mac against files copied from the game, or on the Windows laptop against the paks. Pure Python.
"""
import argparse
import struct
import sys
import zlib

# --- chunk files ------------------------------------------------------------------

CHUNK_CONTROLLER = 0x100D
CHUNK_COMPILED_BONES = 0x2000
CHUNK_CAF_HEADER = 0x3007


def chunks(data):
    """(type, version, id, bytes) for each chunk of a CrCh 0x746 file."""
    sig, _version, count, table = struct.unpack_from("<4sIII", data, 0)
    if sig != b"CrCh":
        raise ValueError("not a CryEngine chunk file: %r" % sig)
    out = []
    for k in range(count):
        ctype, cver, cid, size, offset = struct.unpack_from("<HHIII", data, table + 16 * k)
        out.append((ctype, cver, cid, data[offset:offset + size]))
    return out


def controller_id(joint_name):
    """The id a joint's tracks go by: CRC32 of its name as written (case kept)."""
    return zlib.crc32(joint_name.encode("ascii")) & 0xFFFFFFFF


# --- the skeleton ---------------------------------------------------------------

def bones(chr_data):
    """[(id, name, parent index or -1)] from a .chr's CompiledBones chunk (version 0x0800,
    584 bytes a bone after a 32-byte lead: id at 0, name at 312, parent offset at 572)."""
    for ctype, cver, _cid, body in chunks(chr_data):
        if ctype != CHUNK_COMPILED_BONES:
            continue
        n = (len(body) - 32) // 584
        out = []
        for i in range(n):
            b = body[32 + i * 584:32 + (i + 1) * 584]
            cid = struct.unpack_from("<I", b, 0)[0]
            name = b[312:568].split(b"\0", 1)[0].decode("ascii", "replace")
            off_parent = struct.unpack_from("<i", b, 572)[0]
            out.append((cid, name, i + off_parent if off_parent else -1))
        return out
    raise ValueError("no CompiledBones chunk")


# --- the header cache -----------------------------------------------------------

class ClipHeader(object):
    __slots__ = ("path", "dba_crc", "start", "end", "seconds", "controllers", "flags")

    def __init__(self, entry):
        self.flags = struct.unpack_from("<I", entry, 0)[0]
        self.path = entry[4:entry.index(b"\0", 4)].decode("ascii", "replace")
        _path_crc, self.dba_crc = struct.unpack_from("<II", entry, 260)
        self.start, self.end, self.seconds, self.controllers = struct.unpack_from("<fffI", entry, 300)

    @property
    def stem(self):
        return self.path.rsplit("/", 1)[-1][:-4]


def headers(img_data):
    return [ClipHeader(body) for ctype, _cver, _cid, body in chunks(img_data) if ctype == CHUNK_CAF_HEADER]


def dba_crc(dba_path):
    """The CRC32 a header names its DBA by: of the path lowercased."""
    return zlib.crc32(dba_path.lower().encode("ascii")) & 0xFFFFFFFF


# --- the clips ------------------------------------------------------------------

KEYTIME_FORMATS = ("F32", "UINT16", "Byte", "F32StartStop", "UINT16StartStop", "ByteStartStop", "Bitset")
VALUE_FORMATS = ("NoCompress", "NoCompressQuat", "NoCompressVec3", "ShotInt3Quat", "SmallTreeDWORDQuat",
                 "SmallTree48BitQuat", "SmallTree64BitQuat", "PolarQuat", "SmallTree64BitExtQuat")


def _formats_from_counts(counts, names):
    out = []
    for count, name in zip(counts, names):
        out.extend([name] * count)
    return out


def _keytimes(fmt, size, data, at):
    """Key frames (ints) of a key-time track."""
    if fmt == "F32":
        return [int(round(v)) for v in struct.unpack_from("<%df" % size, data, at)]
    if fmt == "UINT16":
        return list(struct.unpack_from("<%dH" % size, data, at))
    if fmt == "Byte":
        return list(struct.unpack_from("<%dB" % size, data, at))
    if fmt in ("F32StartStop", "UINT16StartStop", "ByteStartStop"):
        code = {"F32StartStop": "<2f", "UINT16StartStop": "<2H", "ByteStartStop": "<2B"}[fmt]
        start, stop = struct.unpack_from(code, data, at)
        return list(range(int(start), int(stop) + 1))
    if fmt == "Bitset":
        # start frame, end frame, key count, then a bit per frame from the start.
        words = struct.unpack_from("<%dH" % size, data, at)
        start, _end, count = words[0], words[1], words[2]
        out = []
        for w, bits in enumerate(words[3:]):
            for j in range(16):
                if bits & (1 << j):
                    out.append(start + 16 * w + j)
        if len(out) != count:
            raise ValueError("bitset says %d keys, has %d bits set" % (count, len(out)))
        return out
    raise ValueError("key-time format %s" % fmt)


def _positions(fmt, size, data, at):
    if fmt in ("NoCompress", "NoCompressVec3"):
        flat = struct.unpack_from("<%df" % (3 * size), data, at)
        return [flat[i:i + 3] for i in range(0, len(flat), 3)]
    raise ValueError("position format %s" % fmt)


class Track(object):
    """One joint's positions over a clip: frames (ints) and (x, y, z) per frame."""
    __slots__ = ("frames", "values")

    def __init__(self, frames, values):
        if len(frames) != len(values):
            raise ValueError("%d key times for %d positions" % (len(frames), len(values)))
        self.frames = frames
        self.values = values

    def at(self, frame):
        """The value at a frame, held from the last key at or before it (the game
        interpolates; for weights read every frame the difference is a hair)."""
        lo, hi = 0, len(self.frames) - 1
        if frame <= self.frames[0]:
            return self.values[0]
        if frame >= self.frames[hi]:
            return self.values[hi]
        while lo < hi - 1:
            mid = (lo + hi) // 2
            if self.frames[mid] <= frame:
                lo = mid
            else:
                hi = mid
        a, b = self.frames[lo], self.frames[hi]
        t = (frame - a) / float(b - a)
        va, vb = self.values[lo], self.values[hi]
        return tuple(va[i] + t * (vb[i] - va[i]) for i in range(3))


class Clip(object):
    __slots__ = ("path", "fps", "start", "end", "tracks", "rotations")

    def __init__(self, path, fps, start, end):
        self.path = path
        self.fps = fps
        self.start = start
        self.end = end
        self.tracks = {}      # controller id -> Track (positions)
        self.rotations = {}   # controller id -> (format, frames, raw bytes), undecoded

    @property
    def stem(self):
        return self.path.rsplit("/", 1)[-1][:-4]

    @property
    def seconds(self):
        return (self.end - self.start) / self.fps

    @property
    def frame_count(self):
        return self.end - self.start + 1


def clips(dba_data, want=None):
    """The clips of a DBA, positions decoded. want: a set of stems to decode, or None for all
    (the others are skipped, which is most of the cost)."""
    out = []
    for ctype, cver, _cid, c in chunks(dba_data):
        if ctype != CHUNK_CONTROLLER:
            continue
        if cver != 0x0905:
            raise ValueError("controller chunk version 0x%04x, not 0x0905" % cver)
        p = 0

        def rd(fmt):
            nonlocal p
            v = struct.unpack_from(fmt, c, p)
            p += struct.calcsize(fmt)
            return v

        n_pos, n_rot, n_time, n_anims = rd("<4I")
        time_sizes = rd("<%dH" % n_time)
        time_fmts = _formats_from_counts(rd("<7I"), KEYTIME_FORMATS)
        pos_sizes = rd("<%dH" % n_pos)
        pos_fmts = _formats_from_counts(rd("<9I"), VALUE_FORMATS)
        rot_sizes = rd("<%dH" % n_rot)
        rot_fmts = _formats_from_counts(rd("<9I"), VALUE_FORMATS)
        time_offs = rd("<%di" % n_time)
        pos_offs = rd("<%di" % n_pos)
        rot_offs = rd("<%di" % (n_rot + 1))
        in_place = n_time > 0 and time_offs[0] < 0
        if in_place:
            padding = rd("<I")[0]
            p += padding
        p = (p + 3) & ~3
        if not in_place:
            # Track data before the entries, offsets from its start.
            base = p
            p += rot_offs[n_rot]
        else:
            base = len(c)

        def track_at(off):
            return base + off

        time_cache = {}

        def keytimes(i):
            if i not in time_cache:
                time_cache[i] = _keytimes(time_fmts[i], time_sizes[i], c, track_at(time_offs[i]))
            return time_cache[i]

        pos_cache = {}

        def positions(i):
            if i not in pos_cache:
                pos_cache[i] = _positions(pos_fmts[i], pos_sizes[i], c, track_at(pos_offs[i]))
            return pos_cache[i]

        entries = []
        entries_start = p
        for _ in range(n_anims):
            n = rd("<H")[0]
            path = c[p:p + n].decode("ascii", "replace")
            p += n
            _flags, _compression, ticks_per_frame, secs_per_tick, start, end = rd("<IIifii")
            p += 132 - 24
            n_foot = rd("<H")[0]
            p += n_foot
            n_ctrl = rd("<H")[0]
            if in_place:
                # From the first entry, not this one: the blocks follow the entries in order.
                ctrl_at = entries_start + rd("<i")[0]
            else:
                ctrl_at = p
                p += 20 * n_ctrl
            fps = 1.0 / (secs_per_tick * ticks_per_frame)
            entries.append((path, fps, start, end, n_ctrl, ctrl_at))

        for path, fps, start, end, n_ctrl, ctrl_at in entries:
            clip = Clip(path, fps, start, end)
            if want is not None and clip.stem not in want:
                continue
            for k in range(n_ctrl):
                cid, pos_time, pos_track, rot_time, rot_track = struct.unpack_from("<5I", c, ctrl_at + 20 * k)
                if pos_track != 0xFFFFFFFF:
                    clip.tracks[cid] = Track(keytimes(pos_time), positions(pos_track))
                if rot_track != 0xFFFFFFFF:
                    fmt = rot_fmts[rot_track]
                    size = rot_sizes[rot_track]
                    clip.rotations[cid] = (fmt, keytimes(rot_time), size, track_at(rot_offs[rot_track]))
            out.append(clip)
    return out


# --- the face -------------------------------------------------------------------

# A pose joint's weight is its translation along x, in metres as the file has it: 0 at
# rest, 0.5 for the pose at half, up to about 0.9 (in the game the probe read it as a
# world offset from the entity, which turned it negative: scripts/autopilot/lipsync_probe2.lua).
def pose_weight(xyz):
    return xyz[0]


# The poses that open or shape the mouth, as the lipsync probe found them moving through
# speech; the pose rig's others are brows, lids, cheeks, nose and the jaw's sideways set.
MOUTH_POSES = (
    "FA_POSE_C_jawDrop", "FA_POSE_C_lipPart", "FA_POSE_C_mouthStretch", "FA_POSE_C_funnel",
    "FA_POSE_C_O", "FA_POSE_C_U", "FA_POSE_C_kiss", "FA_POSE_C_protude", "FA_POSE_C_press",
    "FA_POSE_L_lipStretcher", "FA_POSE_R_lipStretcher", "FA_POSE_L_lowLipDepressor",
    "FA_POSE_R_lowLipDepressor", "FA_POSE_L_upLipRaise", "FA_POSE_R_upLipRaise",
    "FA_POSE_L_cornerPull", "FA_POSE_R_cornerPull", "FA_POSE_L_showTeeth", "FA_POSE_R_showTeeth",
)
# What opens the mouth itself, as against shaping closed lips.
OPEN_POSES = ("FA_POSE_C_jawDrop", "FA_POSE_C_lipPart", "FA_POSE_C_mouthStretch", "FA_POSE_C_funnel",
              "FA_POSE_C_O", "FA_POSE_C_U", "FA_POSE_L_showTeeth", "FA_POSE_R_showTeeth")


def openness(clip, poses=OPEN_POSES):
    """Per frame, the largest weight among the poses that open the mouth."""
    ids = [controller_id(n) for n in poses]
    tracks = [clip.tracks[i] for i in ids if i in clip.tracks]
    out = []
    for f in range(clip.start, clip.end + 1):
        best = 0.0
        for t in tracks:
            w = pose_weight(t.at(f))
            if w > best:
                best = w
        out.append(best)
    return out


def profile(clip, step=0.1, poses=OPEN_POSES):
    """Openness per step of seconds (the largest within each), the clip's whole length."""
    per_frame = openness(clip, poses)
    n = max(1, int(round(clip.seconds / step)))
    out = []
    for k in range(n):
        a = int(k * step * clip.fps)
        b = max(a + 1, int((k + 1) * step * clip.fps))
        out.append(max(per_frame[a:b]) if a < len(per_frame) else 0.0)
    return out


def digits(values, full=1.0):
    """A profile as one digit per step, 0..9 of the way to `full`."""
    return "".join(str(min(9, int(10 * v / full))) if v > 0 else "0" for v in values)


# --- writing a clip ---------------------------------------------------------------
#
# A loose CAF as the game's own loose clips have it (Animations.pak's cinematics; checked
# against alchemy_idle_lookposes_head.caf, 13 Oct 2026): a CrCh 0x746 file whose chunk
# table (16-byte rows: type, version, id, size, offset) sits at offset 16, a MotionParameters
# chunk (0x3002 version 0x0925, 132 bytes: asset flags, compression, ticks per frame, seconds
# per tick, start and end frame, five movement floats, start and end QuatT, eight heel and
# toe plants at -10000) and one controller chunk per joint (0x100d version 0x0829): the
# joint's id, the rotation and position key counts, the formats, then the rotation keys,
# their times, the position keys and theirs, each track padded to four bytes. The pose
# joints only ever have positions (as the shipped facial clips give them), so a shape is a
# position-only controller per pose, three uncompressed floats a key, byte key times.

CAF_VERSION = 0x746
CHUNK_MOTION_PARAMS = 0x3002
FORMAT_NO_COMPRESS_VEC3 = 2
KEYTIME_BYTE = 2
KEYTIME_UINT16 = 1


def write_caf(path, tracks, frames, fps=30.0, loop=False):
    """Write a clip: tracks {controller id: [(x, y, z) per key]}, frames the key frames (the same
    for every track, ints from 0), fps its rate. Every track must have one value per key frame."""
    frames = [int(f) for f in frames]
    if not frames or frames != sorted(frames) or frames[0] != 0:
        raise ValueError("key frames must start at 0 and ascend: %r" % frames)
    time_fmt = KEYTIME_BYTE if frames[-1] < 256 else KEYTIME_UINT16
    time_code = "B" if time_fmt == KEYTIME_BYTE else "H"

    def padded(b):
        return b + b"\0" * (-len(b) % 4)

    bodies = []
    params = struct.pack("<IIifii", 2 if loop else 0, 0, 1, 1.0 / fps, 0, frames[-1])
    params += struct.pack("<5f", 0, 0, 0, 0, 0)
    params += struct.pack("<7f", 0, 0, 0, 1, 0, 0, 0) * 2          # start and end location: identity
    params += struct.pack("<8f", *([-10000.0] * 8))
    assert len(params) == 132
    bodies.append((CHUNK_MOTION_PARAMS, 0x0925, params))
    for cid in sorted(tracks):
        values = tracks[cid]
        if len(values) != len(frames):
            raise ValueError("controller %08x has %d values for %d key frames" % (cid, len(values), len(frames)))
        head = struct.pack("<IHH6B", cid, 0, len(values), 0, 0, FORMAT_NO_COMPRESS_VEC3,
                           1,          # position keys carry their own times
                           time_fmt, 1)
        head += b"\0\0"                                            # to 16 bytes
        pos = padded(b"".join(struct.pack("<3f", *v) for v in values))
        times = padded(struct.pack("<%d%s" % (len(frames), time_code), *frames))
        bodies.append((CHUNK_CONTROLLER, 0x0829, head + pos + times))

    table_at = 16
    data_at = table_at + 16 * len(bodies)
    table = b""
    data = b""
    for k, (ctype, cver, body) in enumerate(bodies):
        table += struct.pack("<HHIII", ctype, cver, k + 1, len(body), data_at + len(data))
        data += padded(body)
    out = struct.pack("<4sIII", b"CrCh", CAF_VERSION, len(bodies), table_at) + table + data
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def pose_clip(path, weights, frames=15, fps=30.0):
    """A clip holding one set of pose weights {joint name: weight} for `frames` frames (two
    keys, the first and the last, both the same), for the mod to blend a mouth shape in with
    StartAnimation and leave it there."""
    tracks = {controller_id(name): [(float(w), 0.0, 0.0)] * 2 for name, w in weights.items()}
    return write_caf(path, tracks, [0, frames - 1], fps)


# --- command line ---------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bones")
    b.add_argument("chr")
    c = sub.add_parser("clips")
    c.add_argument("dba")
    d = sub.add_parser("dump")
    d.add_argument("dba")
    d.add_argument("stem")
    d.add_argument("--chr", help="a .chr, to name the joints")
    m = sub.add_parser("mouth")
    m.add_argument("dba")
    m.add_argument("stem")
    args = ap.parse_args()

    if args.cmd == "bones":
        for cid, name, parent in bones(open(args.chr, "rb").read()):
            print("%08x %-40s parent %d" % (cid, name, parent))
    elif args.cmd == "clips":
        for clip in clips(open(args.dba, "rb").read()):
            print("%-60s %5.2fs %3d fps frames %d..%d, %d position tracks, %d rotation"
                  % (clip.stem, clip.seconds, round(clip.fps), clip.start, clip.end, len(clip.tracks), len(clip.rotations)))
    elif args.cmd == "dump":
        names = {}
        if args.chr:
            names = {cid: name for cid, name, _ in bones(open(args.chr, "rb").read())}
        for clip in clips(open(args.dba, "rb").read(), {args.stem}):
            ids = sorted(clip.tracks, key=lambda i: names.get(i, "%08x" % i))
            print("frame " + " ".join("%12s" % names.get(i, "%08x" % i)[-12:] for i in ids))
            for f in range(clip.start, clip.end + 1):
                print("%5d " % f + " ".join("%12.3f" % pose_weight(clip.tracks[i].at(f)) for i in ids))
    elif args.cmd == "mouth":
        for clip in clips(open(args.dba, "rb").read(), {args.stem}):
            prof = profile(clip)
            print("%s: %.2fs, openness by 100 ms: %s" % (clip.stem, clip.seconds, digits(prof)))
            print("  " + " ".join("%.2f" % v for v in prof))
    return 0


if __name__ == "__main__":
    sys.exit(main())
