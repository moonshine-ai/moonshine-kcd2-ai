#!/usr/bin/env python3
"""Writes mod/assets/pak/Libs/UI/MoonshineCaption.gfx: the conversation caption of our own.

    python3 tools/gen_caption_gfx.py [out.gfx] [--red-plate]

The game's dialogue caption is one HTML text field (hud.gfx, bc.subtitles.inside.tField)
that the dialogue system clears and rewrites at every beat, and that takes itself down a
second after it is set; the mod used to hold its own words in it by rewriting them every
frame, and still lost one now and then (the flicker). This movie is a copy of that
widget's tree -- a sprite at the HUD's bottom centre, a 'sub' clip the game would move,
the same field on the same font with the same drop shadow -- that nothing else writes
to, so the words are set once and stay. The Lua side (emit.cpp, Write) asks the game's
hidden widget where it would put the line (UIAction.GetPos "Subtitles") and puts 'sub'
there, so the caption lands exactly where the game's would.

What was measured to make it pixel-identical (wl1, 10 Nov 2026, runs 9-13):
  - size: the field in hud.gfx says 33 px but fc_setSubtitles overrides it with
    TextFormat.size = Enum.FONT_SIZE_DEFAULT = 28, leading 2 px, line pitch 35.8 px;
  - frame: hud.gfx runs at Stage.scaleMode "noScale" in a fullscreen element and puts
    bc at (Stage.width/2, Stage.height) scaled Stage.height/1080 (SetupElements), which
    is what the DoAction here does for 'all', at load and again on cry_onResize;
  - colour: the speaker's row in ColorScheme.DARK_EMPHASIS #FFEE99 (DARK_POSITIVE
    #66BFFF when it is Henry's own line), set from Lua on the name field's textColor,
    since <font color> inside htmlText is not honoured in these fields;
  - the drop shadow: black, blur 4x3, angle 0.79, distance 2, strength 1.5, 3 passes.

No ActionScript beyond the placement: the words, their rows, the speaker's colour and the
plate's visibility are all set from Lua through the variables MoonshineCaption.xml names.
The movie imports DefaultFont from the game's Libs/UI/gfxfontlib.gfx, so no glyph ships.
"""
import struct, sys, zlib

FONT_PX = 28
LEADING_TWIPS = 40
PLATE_RGBA = (0, 0, 0, 150)


def u8(v): return struct.pack('<B', v)
def u16(v): return struct.pack('<H', v)
def s16(v): return struct.pack('<h', v)
def u32(v): return struct.pack('<I', v)
def s32(v): return struct.pack('<i', v)
def cstr(s): return s.encode('utf-8') + b'\x00'


class Bits:
    def __init__(self): self.bits = []
    def ub(self, n, v):
        for i in range(n - 1, -1, -1): self.bits.append((v >> i) & 1)
    def sb(self, n, v):
        if v < 0: v += 1 << n
        self.ub(n, v)
    def bytes(self):
        b = self.bits + [0] * ((8 - len(self.bits) % 8) % 8)
        return bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def nbits_signed(*vals):
    n = 1
    for v in vals:
        while not (-(1 << (n - 1)) <= v < (1 << (n - 1))): n += 1
    return n


def rect(xmin, xmax, ymin, ymax):
    n = nbits_signed(xmin, xmax, ymin, ymax)
    b = Bits(); b.ub(5, n)
    for v in (xmin, xmax, ymin, ymax): b.sb(n, v)
    return b.bytes()


def matrix(tx=0, ty=0):
    b = Bits()
    b.ub(1, 0); b.ub(1, 0)                        # no scale, no rotate
    n = nbits_signed(tx, ty); b.ub(5, n); b.sb(n, tx); b.sb(n, ty)
    return b.bytes()


def tag(code, body):
    if len(body) < 0x3f: return u16((code << 6) | len(body)) + body
    return u16((code << 6) | 0x3f) + u32(len(body)) + body


def define_shape_rect(cid, w, h, rgba):
    """DefineShape3: one solid fill, no line, a w x h (twips) rectangle at the origin."""
    fills = u8(1) + u8(0x00) + bytes(rgba)
    lines = u8(0)
    b = Bits()
    b.ub(4, 1); b.ub(4, 0)                        # NumFillBits=1, NumLineBits=0
    b.ub(1, 0); b.ub(5, 0b00011)                  # StyleChange: FillStyle0 + MoveTo
    b.ub(5, 1); b.sb(1, 0); b.sb(1, 0)            # move to (0,0)
    b.ub(1, 1)                                    # FillStyle0 = 1
    def line(dx, dy):
        n = max(2, nbits_signed(dx, dy))
        b.ub(1, 1); b.ub(1, 1); b.ub(4, n - 2)    # straight edge
        if dx and dy: b.ub(1, 1); b.sb(n, dx); b.sb(n, dy)
        elif dx: b.ub(1, 0); b.ub(1, 0); b.sb(n, dx)
        else: b.ub(1, 0); b.ub(1, 1); b.sb(n, dy)
    line(w, 0); line(0, h); line(-w, 0); line(0, -h)
    b.ub(6, 0)
    return tag(32, u16(cid) + rect(0, w, 0, h) + fills + lines + b.bytes())


def import_font(url, fid, name):
    """ImportAssets2: the font by its exported name from another movie of the game's."""
    return tag(71, cstr(url) + u8(1) + u8(0) + u16(1) + u16(fid) + cstr(name))


def define_edit_text_html(cid, xmin, xmax, ymin, ymax, fid, height_twips, rgba, text, align=2, leading=40):
    # font, colour, readonly, multiline, wordwrap, outlines, html, noselect, layout
    flags = 1 | 4 | 8 | 32 | 64 | 256 | 512 | 4096 | 8192
    if text: flags |= 128
    body = u16(cid) + rect(xmin, xmax, ymin, ymax) + u16(flags) + u16(fid) + u16(height_twips) + bytes(rgba)
    body += u8(align) + u16(0) + u16(0) + u16(0) + s16(leading)
    body += cstr('')
    if text: body += cstr(text)
    return tag(37, body)


def place(cid, depth, name=None, tx=0, ty=0):
    flags = 0x02 | 0x04
    if name: flags |= 0x20
    body = u8(flags) + u16(depth) + u16(cid) + matrix(tx, ty)
    if name: body += cstr(name)
    return tag(26, body)


def place3_shadow(cid, depth, name, tx, ty):
    """PlaceObject3 with the game's own DropShadow filter bytes."""
    flags, flags2 = 0x02 | 0x04 | 0x20, 0x01
    body = u8(flags) + u8(flags2) + u16(depth) + u16(cid) + matrix(tx, ty) + cstr(name)
    shadow = (u8(0) + bytes([0, 0, 0, 255])
              + struct.pack('<ii', int(4.0 * 65536), int(3.0 * 65536))     # blur x, y
              + struct.pack('<ii', int(0.79 * 65536), int(2.0 * 65536))    # angle, distance
              + struct.pack('<h', int(1.5 * 256))                          # strength
              + u8(0x23))                                                  # composite, 3 passes
    return tag(70, body + u8(1) + shadow)


def sprite(sid, inner_tags):
    return tag(39, u16(sid) + u16(1) + b''.join(inner_tags) + tag(1, b'') + tag(0, b''))


# --- The placement script, AS2 bytecode -------------------------------------------------
def push_str(s): b = cstr(s); return u8(0x96) + u16(len(b) + 1) + u8(0) + b
def push_int(v): return u8(0x96) + u16(5) + u8(7) + s32(v)
GET_VARIABLE, GET_MEMBER, SET_MEMBER = u8(0x1C), u8(0x4E), u8(0x4F)
DIVIDE, MULTIPLY, CALL_FUNCTION, POP = u8(0x0D), u8(0x0C), u8(0x3D), u8(0x17)


def set_member(obj, member, value_code):
    """obj.member = <value_code>, obj a variable name."""
    return push_str(obj) + GET_VARIABLE + push_str(member) + value_code + SET_MEMBER


def define_function(name, params, code):
    body = cstr(name) + u16(len(params)) + b''.join(cstr(p) for p in params) + u16(len(code))
    return u8(0x9B) + u16(len(body)) + body + code


def do_action_frame():
    """Stage.scaleMode = "noScale"; Stage.align = "TL";
    function cry_onResize(w, h, sw, sh) {
        all._x = sw / 2; all._y = sh; all._xscale = all._yscale = sh / 1080 * 100;
    }
    cry_onResize(0, 0, Stage.width, Stage.height);
    -- what hud.gfx's SetupElements does for bc; the engine calls cry_onResize itself
    when the viewport changes (CFlashUIElement::UpdateFlags)."""
    stage = (push_str('Stage') + GET_VARIABLE + push_str('scaleMode') + push_str('noScale') + SET_MEMBER
             + push_str('Stage') + GET_VARIABLE + push_str('align') + push_str('TL') + SET_MEMBER)
    sh = push_str('sh') + GET_VARIABLE
    scale = sh + push_int(1080) + DIVIDE + push_int(100) + MULTIPLY
    body = (set_member('all', '_x', push_str('sw') + GET_VARIABLE + push_int(2) + DIVIDE)
            + set_member('all', '_y', sh)
            + set_member('all', '_xscale', scale)
            + set_member('all', '_yscale', scale))
    fn = define_function('cry_onResize', ['w', 'h', 'sw', 'sh'], body)
    # Arguments are pushed last first, then their count, then the name.
    call = (push_str('Stage') + GET_VARIABLE + push_str('height') + GET_MEMBER
            + push_str('Stage') + GET_VARIABLE + push_str('width') + GET_MEMBER
            + push_int(0) + push_int(0) + push_int(4) + push_str('cry_onResize') + CALL_FUNCTION + POP)
    return tag(12, stage + fn + call + u8(0))


def build(plate_rgba=PLATE_RGBA):
    T = [tag(69, u32(0)), tag(9, bytes([0, 0, 0]))]
    T.append(import_font('gfxfontlib.gfx', 1, 'DefaultFont'))
    # The game's tree: bc(960,1080) > subtitles(-480,-230) > inside > tField(-98,2). Ours:
    # all > sub > { plate, name, text }, 'sub' moved from Lua to the y read off the game's.
    # The plate: 1240 x 100 px at (-100, -12); Lua shows it only when the player has the
    # subtitle background on. The fields: the game's own bounds (-2..1158 x -2..662), the
    # speaker's name in its own field over the text (one line pitch down when there is a
    # name, textY from Lua), both tall enough to wrap, both with the game's shadow.
    T.append(define_shape_rect(10, 1240 * 20, 100 * 20, plate_rgba))
    T.append(sprite(12, [place(10, 1, None, 0, 0)]))
    T.append(define_edit_text_html(11, -2 * 20, 1158 * 20, -2 * 20, 662 * 20, 1, FONT_PX * 20,
                                   (255, 255, 255, 255), '<p align="center"></p>', leading=LEADING_TWIPS))
    T.append(define_edit_text_html(13, -2 * 20, 1158 * 20, -2 * 20, 662 * 20, 1, FONT_PX * 20,
                                   (255, 238, 153, 255), '<p align="center"></p>', leading=LEADING_TWIPS))
    T.append(sprite(20, [place(12, 1, 'plate', -100 * 20, -12 * 20),
                         place3_shadow(13, 2, 'name', -98 * 20, 2 * 20),
                         place3_shadow(11, 3, 'text', -98 * 20, 2 * 20)]))
    T.append(sprite(21, [place(20, 1, 'sub', -480 * 20, -230 * 20)]))
    T.append(place(21, 1, 'all', 960 * 20, 1080 * 20))
    T.append(do_action_frame())
    T += [tag(1, b''), tag(0, b'')]
    body = rect(0, 1920 * 20, 0, 1080 * 20) + u16(30 << 8) + u16(1) + b''.join(T)
    return b'CWS' + u8(8) + u32(8 + len(body)) + zlib.compress(body, 9)


if __name__ == '__main__':
    args = sys.argv[1:]
    plate = PLATE_RGBA
    if '--red-plate' in args:              # a measurement aid: shows where 'sub' lands
        args.remove('--red-plate'); plate = (200, 0, 0, 140)
    out = args[0] if args else 'mod/assets/pak/Libs/UI/MoonshineCaption.gfx'
    data = build(plate)
    open(out, 'wb').write(data)
    print(out, len(data), 'bytes')
