"""Generate the "layout-tiles" schema_version 2 test skin for Metasequoia IME.

The skin exists to check per-layout overrides and nine-slice round / space tiling by eye, so every edge of the
panels carries one marker per tile: count the markers to see how many tiles were drawn. The horizontal panel has
amber dots, the shared (vertical) panel has teal diamonds; corners are pink squares that must never tile or stretch.

All artwork is procedural (Pillow), drawn at 4x and downsampled. Every bitmap is a 2x asset: the manifest's *_dip
sizes are half the pixel sizes.

Requires Pillow (pip install pillow). From examples/skin-examples:
    python tools/gen_layout_tiles.py skins/layout-tiles
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw

SS = 4  # supersampling factor
PANEL = 72  # panel size in pixels; slices are 24 px, so each edge tile is 24 px long
SLICE = 24


def canvas(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))


def finish(img):
    w, h = img.size
    return img.resize((w // SS, h // SS), Image.LANCZOS)


def edge_centers():
    """Centers of the four edge tiles (top, bottom, left, right) and the four corners, in pixels."""
    mid = PANEL / 2
    near, far = SLICE / 2, PANEL - SLICE / 2
    edges = [(mid, near), (mid, far), (near, mid), (far, mid)]
    corners = [(near, near), (far, near), (near, far), (far, far)]
    return edges, corners


def panel(out, fill, rim, marker, corner, shape):
    s = SS
    img = canvas(PANEL, PANEL)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, PANEL * s - 1, PANEL * s - 1], radius=10 * s, fill=rim)
    d.rounded_rectangle([3 * s, 3 * s, (PANEL - 3) * s - 1, (PANEL - 3) * s - 1], radius=8 * s, fill=fill)
    edges, corners = edge_centers()
    r = 6
    for cx, cy in edges:
        x, y = cx * s, cy * s
        if shape == "dot":
            d.ellipse([x - r * s, y - r * s, x + r * s, y + r * s], fill=marker)
        else:
            d.polygon([(x, y - r * s), (x + r * s, y), (x, y + r * s), (x - r * s, y)], fill=marker)
    for cx, cy in corners:
        x, y = cx * s, cy * s
        d.rectangle([x - 5 * s, y - 5 * s, x + 5 * s, y + 5 * s], fill=corner)
    finish(img).save(out)


def badge(out, name, color, vertical):
    """48x48 tag with three bars: horizontal bars for the horizontal-only decoration, vertical bars for the other."""
    s = SS
    size = 48
    img = canvas(size, size)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2 * s, 2 * s, (size - 2) * s - 1, (size - 2) * s - 1], radius=10 * s, fill=color)
    for i in range(3):
        a = (13 + i * 9) * s
        if vertical:
            d.rounded_rectangle([a, 12 * s, a + 4 * s, 36 * s], radius=2 * s, fill=(255, 255, 255, 235))
        else:
            d.rounded_rectangle([12 * s, a, 36 * s, a + 4 * s], radius=2 * s, fill=(255, 255, 255, 235))
    finish(img).save(out / name)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) / "assets"
    out.mkdir(parents=True, exist_ok=True)
    pink, pink_light = (240, 90, 130, 255), (214, 64, 110, 255)
    panel(out / "panel-h.png", (28, 32, 52, 240), (240, 176, 64, 255), (253, 186, 60, 255), pink, "dot")
    panel(out / "panel-h-light.png", (250, 247, 238, 240), (214, 150, 40, 255), (226, 140, 20, 255), pink_light,
          "dot")
    panel(out / "panel.png", (22, 36, 44, 240), (94, 234, 212, 255), (94, 234, 212, 255), pink, "diamond")
    panel(out / "panel-light.png", (240, 250, 247, 240), (22, 163, 127, 255), (22, 163, 127, 255), pink_light,
          "diamond")
    badge(out, "badge-h.png", (226, 140, 20, 255), vertical=False)
    badge(out, "badge-v.png", (22, 163, 127, 255), vertical=True)


if __name__ == "__main__":
    main()
