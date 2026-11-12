"""Draws the DirSize icon (a folder with a pie chart) and writes
src/app/dirsize.ico. An original drawing, MIT like the rest of the project.

Needs Pillow. Run from anywhere:  python scripts/make-icon.py [--preview out.png]

Sizes: Microsoft's minimum for Win32 apps (16, 24, 32, 48, 256, "App icon
construction" on Microsoft Learn) plus Inno Setup's SetupIconFile advice
(64) and the in-between scale sizes 20 and 40.
"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SUPERSAMPLE = 16  # draw large, then shrink: smooth edges at every size

FOLDER_BACK = (214, 150, 30, 255)    # the tab and the back sheet
FOLDER_FRONT = (250, 196, 64, 255)   # the front sheet
PIE_BASE = (255, 255, 255, 255)
PIE_SLICE = (32, 110, 210, 255)
PIE_EDGE = (150, 100, 10, 255)


def draw(size: int) -> Image.Image:
    s = size * SUPERSAMPLE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = s / 32.0  # design grid: 32 x 32 units

    def box(x0, y0, x1, y1):
        return [round(x0 * u), round(y0 * u), round(x1 * u), round(y1 * u)]

    r = 2.0 * u
    # Back sheet with the tab on the upper left.
    d.rounded_rectangle(box(1.5, 4.5, 13.5, 10), radius=r, fill=FOLDER_BACK)
    d.rounded_rectangle(box(1.5, 7, 30.5, 27.5), radius=r, fill=FOLDER_BACK)
    # Front sheet.
    d.rounded_rectangle(box(1.5, 10, 30.5, 28.5), radius=r, fill=FOLDER_FRONT)

    # Pie on the front sheet: a white disc, a blue slice of about 70 %.
    cx, cy, rad = 16.0, 19.25, 7.25
    edge = max(1, round((0.9 if size <= 24 else 0.6) * u))
    d.ellipse(box(cx - rad, cy - rad, cx + rad, cy + rad), fill=PIE_BASE, outline=PIE_EDGE, width=edge)
    inner = rad - 0.6 if size > 24 else rad - 0.9
    d.pieslice(box(cx - inner, cy - inner, cx + inner, cy + inner), start=-90, end=-90 + 252, fill=PIE_SLICE)
    # A gap line from the centre to the 12 o'clock edge keeps the slice
    # readable at small sizes.
    if size >= 32:
        d.line([(cx * u, cy * u), (cx * u, (cy - inner) * u)], fill=PIE_BASE, width=max(1, round(0.5 * u)))

    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="also write a PNG strip of all sizes")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    images = [draw(n) for n in SIZES]
    out = root / "src" / "app" / "dirsize.ico"
    images[-1].save(out, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    print(f"wrote {out.relative_to(root)} ({', '.join(map(str, SIZES))})")

    if args.preview:
        gap = 12
        width = sum(SIZES) + gap * (len(SIZES) + 1)
        strip = Image.new("RGBA", (width * 2, 256 + 2 * gap), (0, 0, 0, 0))
        for half, bg in enumerate([(255, 255, 255, 255), (32, 32, 32, 255)]):
            panel = Image.new("RGBA", (width, 256 + 2 * gap), bg)
            x = gap
            for n, im in zip(SIZES, images):
                panel.alpha_composite(im, (x, gap + 256 - n))
                x += n + gap
            strip.paste(panel, (half * width, 0))
        strip.save(args.preview)
        print(f"wrote preview {args.preview}")


if __name__ == "__main__":
    main()
