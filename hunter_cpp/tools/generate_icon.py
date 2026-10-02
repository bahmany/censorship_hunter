#!/usr/bin/env python3
"""
Generate a proper multi-resolution icon for the Hunter project.

The icon is a shield (security/circumvention) with a magnifying glass (hunting
for working proxies). Drawn procedurally with Pillow so it looks crisp at every
size from 16x16 to 256x256.
"""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

SIZES = [16, 24, 32, 48, 64, 128, 256]
OUT_ICO = os.path.join(os.path.dirname(__file__), "..", "hunter.ico")
OUT_PNG = os.path.join(os.path.dirname(__file__), "..", "hunter_icon.png")


def draw_icon(size: int) -> Image.Image:
    """Draw the Hunter icon at the given pixel size."""
    s = size
    # Use a supersample factor for anti-aliasing on small sizes.
    ss = 4 if s <= 64 else 2 if s <= 128 else 1
    S = s * ss
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    cx, cy = S / 2, S / 2

    # ── Shield ──
    # Shield occupies ~80% of the canvas, centered.
    sw = S * 0.78
    sh = S * 0.88
    sx = cx - sw / 2
    sy = cy - sh / 2 + S * 0.02

    # Shield path: rounded top, pointed bottom.
    r = sw * 0.22  # corner radius for top
    top_y = sy
    bot_y = sy + sh
    mid_y = sy + sh * 0.52

    def shield_path():
        from PIL import ImageDraw
        pts = []
        # Top-left corner (rounded)
        pts.append((sx + r, top_y))
        pts.append((sx + sw - r, top_y))
        # Top-right corner arc
        pts.append((sx + sw, top_y + r))
        # Right side down to mid
        pts.append((sx + sw, mid_y))
        # Curve to bottom point
        pts.append((cx + sw * 0.18, bot_y - sh * 0.12))
        pts.append((cx, bot_y))
        pts.append((cx - sw * 0.18, bot_y - sh * 0.12))
        # Left side up
        pts.append((sx, mid_y))
        pts.append((sx, top_y + r))
        return pts

    pts = shield_path()

    # Shield drop shadow
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.polygon(pts, fill=(0, 0, 0, 90))
    shadow = shadow.filter(ImageFilter.GaussianBlur(S * 0.03))
    img = Image.alpha_composite(img, shadow)
    d = ImageDraw.Draw(img)

    # Shield gradient fill (dark teal → lighter teal)
    grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    # Draw shield as a mask, then fill with vertical gradient.
    mask = Image.new("L", (S, S), 0)
    md = ImageDraw.Draw(mask)
    md.polygon(pts, fill=255)
    # Build gradient.
    for y in range(int(top_y), int(bot_y)):
        t = (y - top_y) / (bot_y - top_y)
        # Top: deep teal #0d7377 → bottom: bright cyan #14e8e8
        r_c = int(13 + (20 - 13) * t)
        g_c = int(115 + (232 - 115) * t)
        b_c = int(119 + (232 - 119) * t)
        gd.line([(0, y), (S, y)], fill=(r_c, g_c, b_c, 255))
    grad.putalpha(mask)
    img = Image.alpha_composite(img, grad)
    d = ImageDraw.Draw(img)

    # Shield border (lighter outline)
    lw = max(1, int(S * 0.025))
    d.line(pts + [pts[0]], fill=(180, 255, 255, 220), width=lw)

    # ── Magnifying glass (hunter) ──
    # Circle in upper-center of shield, handle to lower-right.
    glass_cx = cx
    glass_cy = cy - sh * 0.08
    glass_r = sw * 0.22
    handle_len = sw * 0.20
    handle_angle = math.radians(45)  # down-right

    # Handle (thick line)
    hx1 = glass_cx + glass_r * 0.7 * math.cos(handle_angle)
    hy1 = glass_cy + glass_r * 0.7 * math.sin(handle_angle)
    hx2 = hx1 + handle_len * math.cos(handle_angle)
    hy2 = hy1 + handle_len * math.sin(handle_angle)
    handle_w = max(2, int(glass_r * 0.28))
    d.line([(hx1, hy1), (hx2, hy2)], fill=(255, 255, 255, 240), width=handle_w)
    # Round handle cap
    d.ellipse(
        [hx2 - handle_w / 2, hy2 - handle_w / 2, hx2 + handle_w / 2, hy2 + handle_w / 2],
        fill=(255, 255, 255, 240),
    )

    # Glass ring (white, thick)
    ring_w = max(2, int(glass_r * 0.22))
    d.ellipse(
        [glass_cx - glass_r, glass_cy - glass_r, glass_cx + glass_r, glass_cy + glass_r],
        outline=(255, 255, 255, 245),
        width=ring_w,
    )
    # Glass lens (semi-transparent white fill)
    d.ellipse(
        [
            glass_cx - glass_r + ring_w * 0.5,
            glass_cy - glass_r + ring_w * 0.5,
            glass_cx + glass_r - ring_w * 0.5,
            glass_cy + glass_r - ring_w * 0.5,
        ],
        fill=(255, 255, 255, 40),
    )
    # Glass highlight (small white dot, upper-left)
    hl_r = glass_r * 0.18
    hl_x = glass_cx - glass_r * 0.35
    hl_y = glass_cy - glass_r * 0.35
    d.ellipse(
        [hl_x - hl_r, hl_y - hl_r, hl_x + hl_r, hl_y + hl_r],
        fill=(255, 255, 255, 200),
    )

    # Downsample with anti-aliasing.
    if ss > 1:
        img = img.resize((s, s), Image.LANCZOS)
    return img


def main():
    images = [draw_icon(s) for s in SIZES]

    # Save PNG (256 for Linux window icon / .desktop file)
    images[-1].save(OUT_PNG)
    print(f"Saved {OUT_PNG} ({images[-1].size[0]}x{images[-1].size[1]})")

    # Save ICO (all sizes)
    images[-1].save(OUT_ICO, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"Saved {OUT_ICO} with sizes {SIZES}")


if __name__ == "__main__":
    main()
