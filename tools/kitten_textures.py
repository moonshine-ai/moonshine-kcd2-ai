#!/usr/bin/env python3
"""Kitten diffuse textures for the shipped cat: the game's four cat coats (grey tabby,
ginger, white, black) made younger, for the kitten character definitions under
mod/assets/pak/Objects/characters/animals/cat/ (moonshine_kitten_*.cdf, each the shipped
cat.chr + cat_body.skin under a copied .mtl whose body diffuse is one of these and whose
eyes are a kitten's blue).

What changes, and why. A kitten is scaled down in Lua (0.55-0.6 of the adult reads as a
few months old), but a small cat is still an adult cat until its coat and eyes say
otherwise: kitten fur is down, not guard hair, so the strand detail is softened; the coat
is paler and its markings lower in contrast; ginger kittens are peach rather than orange,
black ones a soft charcoal with a little brown in it; and until about seven weeks every
kitten's eyes are blue, which the .mtl does (IrisColor), not this script. The normal,
specular and detail maps stay the shipped ones, so the fur keeps its relief and sheen.

The shipped diffuses come out of IPL_Characters-part0.pak as split .dds (the .dds holds
the header and the smallest mips, .dds.1..N the larger ones). The grey tabby and the
black are BC1; the ginger and the white are BC7 with an alpha the Hair shader uses, which
nothing on the Mac decodes correctly, so the decode is done once with Blender on wl1
(its OpenImageIO reads BC7) and this script starts from the PNGs:

    ssh wl1 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\\Users\\windo\\autopilot\\wl1_findinpak.ps1 -Pattern "animals/cat/cat_body_.*diff" -PakMask IPL_Characters-part0.pak -Out C:\\Users\\windo\\autopilot\\catx -Max 200'
    # join each split .dds (header + parts largest-first + small mips), then in Blender:
    #   img = bpy.data.images.load(path); img.filepath_raw = path[:-4] + '.png'; img.file_format = 'PNG'; img.save()
    python3 tools/kitten_textures.py --shipped /tmp/kitten/dec --out /tmp/kitten/tex

The output is one RGBA .tif per coat at 1024 (the adult variants' own size; the kitten
is half the adult's screen size anyway). They are compiled on wl1 with the modding
tools' rc.exe -- /preset=Albedo for the tabby and black (BC1, as shipped) and
/preset=AlbedoWithGenericAlpha for the ginger and white (whose alpha is kept as shipped)
-- and the .dds copied beside the .mtl in mod/assets/pak. A mod may ship a single
unsplit .dds.
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageFilter

SIZE = 1024

# Per coat: the shipped diffuse it starts from, and how it is made a kitten's.
#   soften   how much of a gently blurred copy replaces the strand detail (0..1)
#   lift     gamma on luminance, < 1 brightens the midtones
#   contrast scale of the deviation from the coat's mean colour (< 1 flattens markings)
#   sat      saturation scale
#   tint     per-channel multiplier after the rest, (r, g, b)
#   alpha    keep the shipped alpha channel (the Hair shader reads it on these two coats)
COATS = {
    "tabby":  dict(src="cat_body_diff.png",        soften=0.70, lift=0.72, contrast=0.72, sat=0.90, tint=(1.04, 1.00, 0.95), alpha=False),
    "ginger": dict(src="cat_body_ginger_diff.png", soften=0.70, lift=0.68, contrast=0.70, sat=0.62, tint=(1.04, 1.00, 0.97), alpha=True),
    "white":  dict(src="cat_body_white_diff.png",  soften=0.60, lift=0.85, contrast=0.80, sat=0.85, tint=(1.03, 1.00, 0.96), alpha=True),
    "black":  dict(src="cat_body_black_diff.png",  soften=0.70, lift=0.74, contrast=0.85, sat=0.90, tint=(1.02, 1.00, 0.98), alpha=False),
}
BLUR_RADIUS = 2.2   # texels at 1024: a kitten's down, not a blur


def kitten(rgba, p):
    rgb = rgba[:, :, :3].astype(np.float32) / 255.0
    # soften the guard hair: blend toward a blurred copy
    soft = np.asarray(Image.fromarray((rgb * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(BLUR_RADIUS))).astype(np.float32) / 255.0
    rgb = rgb * (1 - p["soften"]) + soft * p["soften"]
    # lift the midtones on luminance, keeping the hue
    lum = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    lifted = np.power(np.clip(lum, 1e-4, 1), p["lift"])
    rgb = rgb * (lifted / np.maximum(lum, 1e-4))[:, :, None]
    # flatten the markings toward the coat's mean
    mean = rgb.reshape(-1, 3).mean(0)
    rgb = mean + (rgb - mean) * p["contrast"]
    # saturation
    lum = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    rgb = lum[:, :, None] + (rgb - lum[:, :, None]) * p["sat"]
    rgb = rgb * np.array(p["tint"], dtype=np.float32)
    out = np.empty_like(rgba)
    out[:, :, :3] = (np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    out[:, :, 3] = rgba[:, :, 3] if p["alpha"] else 255
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--shipped", required=True, help="folder with the decoded shipped cat_body_*diff.png (RGBA)")
    ap.add_argument("--out", required=True, help="folder for moonshine_kitten_<coat>_diff.tif")
    ap.add_argument("--only", nargs="*", choices=sorted(COATS), help="just these coats")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for coat, p in COATS.items():
        if args.only and coat not in args.only:
            continue
        img = Image.open(os.path.join(args.shipped, p["src"])).convert("RGBA")
        if img.size != (SIZE, SIZE):
            img = img.resize((SIZE, SIZE), Image.LANCZOS)
        out = kitten(np.asarray(img), p)
        path = os.path.join(args.out, "moonshine_kitten_%s_diff.tif" % coat)
        # RGBA either way: rc's Albedo preset drops the (opaque) alpha, AlbedoWithGenericAlpha keeps it
        Image.fromarray(out, "RGBA").save(path, compression="tiff_lzw")
        mean = out[:, :, :3].reshape(-1, 3).mean(0)
        print("%-7s %s  mean rgb (%.0f, %.0f, %.0f)" % (coat, path, *mean))


if __name__ == "__main__":
    main()
