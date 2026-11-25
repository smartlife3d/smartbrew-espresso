#!/usr/bin/env python3
"""Generate the espresso screen-saver art used by the Waveshare display and the
Android client.

One drawing, two consumers:

  * LVGL (Waveshare ESP32-S3 3.5") draws 8-bit *alpha masks* and tints each of
    them with ``lv_obj_set_style_img_recolor()``.  That keeps the whole sprite
    set inside the flash budget and costs no LVGL heap (LV_MEM_SIZE is only
    64 kB on that board).
  * Android composites the same layers into a single RGBA PNG that is drawn
    with a ``Bitmap``.

The house palette comes from the web cockpit (``data/index.html``):

  espresso  rgba(38,14,4) -> rgba(69,26,8) -> rgba(120,53,15) -> rgba(180,83,9)
  crema     rgba(253,224,71) / rgba(245,158,11) / rgba(180,83,9)
  OLED      #07090E

Usage::

    python3 tools/gen_screensaver_art.py            # regenerate everything
    python3 tools/gen_screensaver_art.py --preview  # also refresh the preview

The generated files are checked in, so a normal build never needs Python.
"""

from __future__ import annotations

import argparse
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw

try:
    from PIL import ImageFilter
except ImportError:  # pragma: no cover - Pillow always ships ImageFilter
    ImageFilter = None

# --------------------------------------------------------------------------
# Paths
# --------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ANDROID_RES = os.path.join(
    REPO_ROOT, "android-app", "app", "src", "main", "res", "drawable-nodpi"
)
WAVESHARE_SRC = os.path.join(REPO_ROOT, "display-waveshare-3.5", "src")
WAVESHARE_INC = os.path.join(REPO_ROOT, "display-waveshare-3.5", "include")
ART_DIR = os.path.join(REPO_ROOT, "tools", "art")

# --------------------------------------------------------------------------
# Geometry
#
# Everything is authored in a 96x96 "design space" and then rasterised at a
# higher resolution before being downsampled, which gives clean anti-aliased
# edges for both the PNG and the 8-bit alpha masks.
# --------------------------------------------------------------------------

CUP_W = 96
CUP_H = 96
SS = 4  # supersampling factor

GLOW_W = 128
GLOW_H = 128

STEAM_W = 40
STEAM_H = 56

ANDROID_SCALE = 4  # design space -> Android PNG pixels (96 * 4 = 384 px)

# Cup landmarks (design space)
RIM_CX, RIM_CY = 43.0, 29.0
RIM_RX, RIM_RY = 23.0, 7.2
LIQ_RX, LIQ_RY = 18.5, 5.6
BODY_TOP_L, BODY_TOP_R = 19.0, 67.0
BODY_BOT_L, BODY_BOT_R = 24.0, 62.0
BODY_BOT_Y = 62.0
BASE_CX, BASE_CY = 43.0, 62.0
BASE_RX, BASE_RY = 18.0, 5.0
HANDLE_CX, HANDLE_CY = 69.0, 41.0
HANDLE_RX, HANDLE_RY = 15.5, 14.0
HANDLE_HOLE_CX, HANDLE_HOLE_CY = 71.0, 41.0
HANDLE_HOLE_RX, HANDLE_HOLE_RY = 7.5, 6.5
SAUCER_CX, SAUCER_CY = 43.0, 72.0
SAUCER_RX, SAUCER_RY = 34.0, 8.5

# House palette
CERAMIC = (237, 224, 210)       # #EDE0D2
CERAMIC_SHADE = (150, 106, 70)   # #966A46 - warm shadow on the ceramic
ESPRESSO = (120, 53, 15)         # #78350F - rgba(120,53,15)
CREMA = (245, 158, 11)           # #F59E0B - rgba(245,158,11)
CREMA_HI = (253, 224, 71)        # #FDE047 - rgba(253,224,71)
STEAM = (245, 235, 222)          # #F5EBDE


def _blank(w: int, h: int) -> Image.Image:
    return Image.new("L", (w, h), 0)


def _ellipse_box(cx, cy, rx, ry):
    return [cx - rx, cy - ry, cx + rx, cy + ry]


def _cup_body_mask(scale: int) -> Image.Image:
    """Solid silhouette of cup + saucer (no coffee, no shading)."""
    w, h = CUP_W * scale, CUP_H * scale
    img = _blank(w, h)
    d = ImageDraw.Draw(img)

    def s(v):
        return v * scale

    # Saucer first so the cup body sits on top of it.
    d.ellipse(_ellipse_box(s(SAUCER_CX), s(SAUCER_CY), s(SAUCER_RX), s(SAUCER_RY)), fill=255)

    # Handle (outer ring); the hole is punched out afterwards.
    d.ellipse(
        _ellipse_box(s(HANDLE_CX), s(HANDLE_CY), s(HANDLE_RX), s(HANDLE_RY)), fill=255
    )

    # Cup body.
    d.polygon(
        [
            (s(BODY_TOP_L), s(RIM_CY)),
            (s(BODY_BOT_L), s(BODY_BOT_Y)),
            (s(BODY_BOT_R), s(BODY_BOT_Y)),
            (s(BODY_TOP_R), s(RIM_CY)),
        ],
        fill=255,
    )
    # Rounded bottom of the cup.
    d.ellipse(_ellipse_box(s(BASE_CX), s(BASE_CY), s(BASE_RX), s(BASE_RY)), fill=255)
    # Rim ellipse.
    d.ellipse(_ellipse_box(s(RIM_CX), s(RIM_CY), s(RIM_RX), s(RIM_RY)), fill=255)

    # Punch the handle hole back out - it must stay transparent so the cup
    # reads as a ring, not a blob.
    d.ellipse(
        _ellipse_box(
            s(HANDLE_HOLE_CX), s(HANDLE_HOLE_CY), s(HANDLE_HOLE_RX), s(HANDLE_HOLE_RY)
        ),
        fill=0,
    )
    return img


def _cup_shade_mask(scale: int) -> Image.Image:
    """Soft shading on the right-hand side of the ceramic."""
    w, h = CUP_W * scale, CUP_H * scale
    body = _cup_body_mask(scale)

    # Horizontal ramp: bright on the left, dark on the right.
    ramp = Image.new("L", (w, h), 0)
    rd = ImageDraw.Draw(ramp)
    for x in range(w):
        t = x / max(1, w - 1)
        # Only the outer 45 % of the ramp carries real shadow.
        v = int(255 * (0.10 + 0.72 * max(0.0, (t - 0.45) / 0.55) ** 1.35))
        rd.line([(x, 0), (x, h)], fill=v)

    shade = ImageChops.multiply(body, ramp)
    return shade


def _liquid_mask(scale: int) -> Image.Image:
    """The espresso surface seen through the rim."""
    w, h = CUP_W * scale, CUP_H * scale
    img = _blank(w, h)
    d = ImageDraw.Draw(img)
    d.ellipse(
        _ellipse_box(RIM_CX * scale, RIM_CY * scale, LIQ_RX * scale, LIQ_RY * scale),
        fill=255,
    )
    return img


def _crema_mask(scale: int) -> Image.Image:
    """The caramelised crema swirl floating on the espresso."""
    w, h = CUP_W * scale, CUP_H * scale
    img = _blank(w, h)
    d = ImageDraw.Draw(img)

    cx, cy = RIM_CX * scale, RIM_CY * scale

    # Main crema pool, slightly off-centre so it looks like it settled.
    d.ellipse(_ellipse_box(cx, cy, 13.5 * scale, 3.9 * scale), fill=225)

    # Tiger stripes.
    stripe_w = max(1, int(0.9 * scale))
    for k, (rx, ry, start, end) in enumerate(
        [
            (15.0, 4.4, 185, 355),
            (11.0, 3.2, 190, 350),
            (7.5, 2.2, 200, 340),
        ]
    ):
        d.arc(
            _ellipse_box(cx, cy, rx * scale, ry * scale),
            start=start,
            end=end,
            fill=255,
            width=stripe_w if k == 0 else max(1, stripe_w - 1),
        )
    return img


def _crema_hi_mask(scale: int) -> Image.Image:
    """Small specular highlight on the crema."""
    w, h = CUP_W * scale, CUP_H * scale
    img = _blank(w, h)
    d = ImageDraw.Draw(img)
    d.ellipse(
        _ellipse_box(
            (RIM_CX - 3.0) * scale, (RIM_CY - 0.6) * scale, 5.6 * scale, 1.7 * scale
        ),
        fill=255,
    )
    return img


def _steam_mask(scale: int) -> Image.Image:
    """A single soft curl of steam."""
    w, h = STEAM_W * scale, STEAM_H * scale
    img = _blank(w, h)
    d = ImageDraw.Draw(img)

    def s(v):
        return v * scale

    # Two overlapping curls.  They are drawn as soft, low-alpha strokes and
    # then blurred so the result reads as vapour rather than as a line.  Keep
    # them thin: the curl sits right above the rim of the cup and should look
    # like the first wisp of steam, not like a plume.
    curls = [
        # (cx, y_top, height, sway, width, alpha)
        (19, 47, 26, 6.0, 3.4, 205),
        (22, 30, 20, -5.0, 2.8, 165),
        (18, 16, 12, 3.5, 2.2, 120),
    ]
    for cx, y_top, height, sway, width, alpha in curls:
        pts = []
        steps = 64
        for i in range(steps + 1):
            t = i / steps
            y = s(y_top) - t * s(height)
            # Sinusoidal sway, damped towards the tip.
            x = s(cx) + math.sin(t * math.pi * 1.35) * s(sway) * (1.0 - 0.30 * t)
            pts.append((x, y))
        d.line(pts, fill=alpha, width=int(round(width * scale)), joint="curve")

    if ImageFilter is not None:
        img = img.filter(ImageFilter.GaussianBlur(radius=0.9 * scale))
    return img


def _glow_mask() -> Image.Image:
    """Radial falloff used as the warm halo behind the cup."""
    w, h = GLOW_W, GLOW_H
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    radius = min(w, h) / 2.0
    img = _blank(w, h)
    px = img.load()
    for y in range(h):
        dy = (y - cy) / radius
        for x in range(w):
            dx = (x - cx) / radius
            d = math.sqrt(dx * dx + dy * dy)
            if d >= 1.0:
                continue
            # Soft core with a long, dim tail.
            v = (1.0 - d) ** 2.6
            px[x, y] = int(255 * v)
    return img


def _downscale(img: Image.Image, w: int, h: int) -> Image.Image:
    return img.resize((w, h), Image.LANCZOS)


# --------------------------------------------------------------------------
# Layer table
# --------------------------------------------------------------------------

# name -> (mask builder, tint colour, LVGL recolour)
CUP_LAYERS = [
    ("cup", _cup_body_mask, CERAMIC, 0xEDE0D2),
    ("cup_shade", _cup_shade_mask, CERAMIC_SHADE, 0x966A46),
    ("liquid", _liquid_mask, ESPRESSO, 0x78350F),
    ("crema", _crema_mask, CREMA, 0xF59E0B),
    ("crema_hi", _crema_hi_mask, CREMA_HI, 0xFDE047),
]


def build_cup_masks(scale: int):
    """Return {name: L-mode alpha mask at the design resolution}."""
    masks = {}
    for name, builder, _tint, _lv in CUP_LAYERS:
        masks[name] = _downscale(builder(scale), CUP_W, CUP_H)
    return masks


def build_steam_mask(scale: int) -> Image.Image:
    return _downscale(_steam_mask(scale), STEAM_W, STEAM_H)


def build_glow_mask() -> Image.Image:
    return _glow_mask()


def composite_rgba(mask: Image.Image, colour) -> Image.Image:
    """Turn an alpha mask into an RGBA image tinted with ``colour``."""
    rgba = Image.new("RGBA", mask.size, colour + (0,))
    rgba.putalpha(mask)
    return rgba


def compose_cup(masks, scale: int) -> Image.Image:
    """Composite the cup layers into a single RGBA image."""
    out = Image.new("RGBA", (CUP_W * scale, CUP_H * scale), (0, 0, 0, 0))
    for name, _builder, tint, _lv in CUP_LAYERS:
        mask = masks[name].resize((CUP_W * scale, CUP_H * scale), Image.LANCZOS)
        out = Image.alpha_composite(out, composite_rgba(mask, tint))
    return out


# --------------------------------------------------------------------------
# Emitters
# --------------------------------------------------------------------------


def emit_lvgl_sprites(masks, steam_mask, glow_mask) -> str:
    """Render the alpha masks as an LVGL C source file."""
    sprites = []
    for name, _builder, _tint, _lv in CUP_LAYERS:
        sprites.append((f"saver_{name}", masks[name], "8-bit alpha mask, tinted with img_recolor"))
    sprites.append(("saver_steam", steam_mask, "8-bit alpha mask, tinted with img_recolor"))
    sprites.append(("saver_glow", glow_mask, "8-bit alpha mask, radial falloff"))

    out = []
    out.append("/*")
    out.append(" * Screen-saver sprites for the espresso floating animation.")
    out.append(" *")
    out.append(" * AUTO-GENERATED by tools/gen_screensaver_art.py - do not edit by hand.")
    out.append(" *")
    out.append(" * Every sprite is an 8-bit alpha mask.  LVGL draws an ALPHA_8BIT image as a")
    out.append(" * solid colour modulated by the mask, so the tint is supplied at runtime with")
    out.append(" * lv_obj_set_style_img_recolor().  Keeping the art as masks costs one byte")
    out.append(" * per pixel and no LVGL heap at all.")
    out.append(" */")
    out.append("")
    out.append('#include "screensaver_art.h"')
    out.append("")

    for name, mask, note in sprites:
        w, h = mask.size
        data = list(mask.tobytes())
        out.append(f"/* {name}: {w}x{h} - {note} */")
        out.append(f"static const uint8_t {name}_map[] = {{")
        for i in range(0, len(data), 16):
            chunk = data[i : i + 16]
            out.append("    " + " ".join(f"0x{b:02x}," for b in chunk))
        out.append("};")
        out.append("")

    for name, mask, _note in sprites:
        w, h = mask.size
        out.append(f"const lv_img_dsc_t img_{name} = {{")
        out.append("    .header = {")
        out.append("        .cf = LV_IMG_CF_ALPHA_8BIT,")
        out.append("        .always_zero = 0,")
        out.append("        .reserved = 0,")
        out.append(f"        .w = {w},")
        out.append(f"        .h = {h},")
        out.append("    },")
        out.append(f"    .data_size = sizeof({name}_map),")
        out.append(f"    .data = {name}_map,")
        out.append("};")
        out.append("")

    return "\n".join(out)


def emit_lvgl_header(sprites) -> str:
    out = []
    out.append("#pragma once")
    out.append("#include <lvgl.h>")
    out.append("")
    out.append("#ifdef __cplusplus")
    out.append('extern "C" {')
    out.append("#endif")
    out.append("")
    out.append("// AUTO-GENERATED by tools/gen_screensaver_art.py - do not edit by hand.")
    for name, mask, _note in sprites:
        w, h = mask.size
        out.append(f"// {name}: {w}x{h}")
        out.append(f"extern const lv_img_dsc_t img_{name};")
    out.append("")
    out.append("#ifdef __cplusplus")
    out.append("}")
    out.append("#endif")
    out.append("")
    return "\n".join(out)


def emit_android_pngs(cup_rgba, steam_mask) -> None:
    os.makedirs(ANDROID_RES, exist_ok=True)
    cup_path = os.path.join(ANDROID_RES, "espresso_cup.png")
    cup_rgba.save(cup_path, optimize=True)
    print(f"  {os.path.relpath(cup_path, REPO_ROOT)}")

    # The steam wisp is tiny in design space; give Android the same 4x
    # oversampling the cup gets so it stays crisp when scaled up.
    steam_rgba = composite_rgba(
        steam_mask.resize((STEAM_W * ANDROID_SCALE, STEAM_H * ANDROID_SCALE), Image.LANCZOS),
        STEAM,
    )
    steam_path = os.path.join(ANDROID_RES, "espresso_steam.png")
    steam_rgba.save(steam_path, optimize=True)
    print(f"  {os.path.relpath(steam_path, REPO_ROOT)}")


def _background(w: int, h: int, top, bottom) -> Image.Image:
    bg = Image.new("RGB", (w, h), top)
    d = ImageDraw.Draw(bg)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line(
            [(0, y), (w, y)],
            fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)),
        )
    return bg.convert("RGBA")


def _tinted(mask: Image.Image, colour, opa: float = 1.0) -> Image.Image:
    """Alpha mask tinted with ``colour``; ``opa`` scales the mask."""
    m = mask if opa >= 1.0 else mask.point(lambda v: int(v * opa))
    return composite_rgba(m, colour)


def _paste(base: Image.Image, sprite: Image.Image, x: int, y: int) -> Image.Image:
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    layer.paste(sprite, (x, y), sprite)
    return Image.alpha_composite(base, layer)


def preview_android(masks, steam_mask, glow_mask) -> Image.Image:
    """Mirrors ScreensaverView.onDraw()."""
    W, H = 480, 320
    bg = _background(W, H, (7, 9, 14), (0x2E, 0x14, 0x09))

    cup_size = min(W, H) * 0.42
    x_min, x_max = cup_size * 0.35, W - cup_size * 1.35
    y_min, y_max = cup_size * 0.45, H - cup_size * 1.45
    cup_left = x_min + (x_max - x_min) * 0.62
    cup_top = y_min + (y_max - y_min) * 0.35

    glow = _tinted(glow_mask, CREMA, 150 / 255.0).resize(
        (int(cup_size * 1.334), int(cup_size * 1.334)), Image.LANCZOS
    )
    bg = _paste(
        bg, glow,
        int(cup_left + cup_size * 0.5 - glow.width / 2),
        int(cup_top + cup_size * 0.5 - glow.height / 2),
    )

    scale = cup_size / 96.0
    steam_full = _tinted(steam_mask, STEAM)
    for alpha, rise, sway in [(0.41, -16.0, 23.0), (0.21, -26.0, 27.0)]:
        wisp = steam_full.copy()
        wisp.putalpha(wisp.getchannel("A").point(lambda v: int(v * alpha)))
        wisp = wisp.resize(
            (int(steam_full.width * scale), int(steam_full.height * scale)), Image.LANCZOS
        )
        bg = _paste(bg, wisp, int(cup_left + sway * scale), int(cup_top + rise * scale))

    cup = compose_cup(masks, 4).resize((int(cup_size), int(cup_size)), Image.LANCZOS)
    bg = _paste(bg, cup, int(cup_left), int(cup_top))

    d = ImageDraw.Draw(bg)
    d.text((W / 2, H - 22), "SMARTBREW", fill=(0x8A, 0x5A, 0x2B))
    return bg.convert("RGB")


def preview_waveshare(masks, steam_mask, glow_mask) -> Image.Image:
    """Mirrors the LVGL layer stack of display-waveshare-3.5/src/screensaver.cpp."""
    W, H = 480, 320
    bg = _background(W, H, (0x07, 0x09, 0x0E), (0x2E, 0x14, 0x09))

    # The floating group sits at (34, 42) and is 96 x 96.
    gx, gy = 34, 42

    bg = _paste(bg, _tinted(glow_mask, CREMA, 95 / 255.0), gx - 16, gy - 16)

    for name, tint in (
        ("cup", CERAMIC),
        ("cup_shade", CERAMIC_SHADE),
        ("liquid", ESPRESSO),
        ("crema", CREMA),
        ("crema_hi", CREMA_HI),
    ):
        bg = _paste(bg, _tinted(masks[name], tint), gx, gy)

    for i, (alpha, rise, sway) in enumerate([(0.41, -16, 23), (0.21, -26, 27)]):
        wisp = _tinted(steam_mask, CERAMIC)
        wisp.putalpha(wisp.getchannel("A").point(lambda v: int(v * alpha)))
        bg = _paste(bg, wisp, gx + sway, gy + rise)

    d = ImageDraw.Draw(bg)
    d.text((W / 2, H - 22), "TOUCH TO RESUME", fill=(0x8A, 0x5A, 0x2B))
    return bg.convert("RGB")


def emit_preview(masks, steam_mask, glow_mask) -> None:
    """Mock of the finished screen saver on both platforms.

    The left half is what the Android overlay draws, the right half mirrors
    the LVGL layer stack of ``display-waveshare-3.5/src/screensaver.cpp``
    (same sprites, same tints, same opacities, same order).
    """
    android = preview_android(masks, steam_mask, glow_mask)
    waveshare = preview_waveshare(masks, steam_mask, glow_mask)

    canvas = Image.new(
        "RGB", (android.width + waveshare.width + 12, android.height), (0, 0, 0)
    )
    canvas.paste(android, (0, 0))
    canvas.paste(waveshare, (android.width + 12, 0))

    os.makedirs(ART_DIR, exist_ok=True)
    path = os.path.join(ART_DIR, "screensaver_preview.png")
    canvas.save(path, optimize=True)
    print(f"  {os.path.relpath(path, REPO_ROOT)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--preview", action="store_true", help="also refresh tools/art/screensaver_preview.png"
    )
    args = ap.parse_args()

    print("Generating screen-saver art...")

    masks = build_cup_masks(SS)
    steam_mask = build_steam_mask(SS)
    glow_mask = build_glow_mask()

    # --- LVGL sprites -----------------------------------------------------
    sprites = []
    for name, _builder, _tint, _lv in CUP_LAYERS:
        sprites.append((f"saver_{name}", masks[name], "8-bit alpha mask, tinted with img_recolor"))
    sprites.append(("saver_steam", steam_mask, "8-bit alpha mask, tinted with img_recolor"))
    sprites.append(("saver_glow", glow_mask, "8-bit alpha mask, radial falloff"))

    c_path = os.path.join(WAVESHARE_SRC, "screensaver_art.c")
    with open(c_path, "w", encoding="utf-8") as fh:
        fh.write(emit_lvgl_sprites(masks, steam_mask, glow_mask))
    print(f"  {os.path.relpath(c_path, REPO_ROOT)}")

    h_path = os.path.join(WAVESHARE_INC, "screensaver_art.h")
    with open(h_path, "w", encoding="utf-8") as fh:
        fh.write(emit_lvgl_header(sprites))
    print(f"  {os.path.relpath(h_path, REPO_ROOT)}")

    # --- Android ----------------------------------------------------------
    cup_rgba = compose_cup(masks, ANDROID_SCALE)
    emit_android_pngs(cup_rgba, steam_mask)

    if args.preview:
        emit_preview(masks, steam_mask, glow_mask)

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
