"""Generate FormaView's app icon: a textured cube in a preview viewfinder over an animation timeline.

One geometry source drives the SVG and every ICO layer.
24-32 px drop the viewfinder corners; 16-20 px keep only the cube.
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1] / "src/host/icons"
TILE_TOP, TILE_BOTTOM, TILE_EDGE = "#3a3a41", "#1f1f23", "#55555e"
TOP, LEFT, RIGHT = "#f6b274", "#d8783a", "#a84a1c"
CREAM, GUIDE, TRACK = "#fff1d6", "#9a9aa2", "#4a4a52"
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def cube_faces(cx, cy, s, checker):
    k = 0.866 * s
    T = [(cx, cy - s), (cx + k, cy - s / 2), (cx, cy), (cx - k, cy - s / 2)]
    L = [(cx - k, cy - s / 2), (cx, cy), (cx, cy + s), (cx - k, cy + s / 2)]
    R = [(cx, cy), (cx + k, cy - s / 2), (cx + k, cy + s / 2), (cx, cy + s)]
    shapes = [("poly", L, LEFT), ("poly", R, RIGHT), ("poly", T, TOP)]
    if checker:  # 2x2 texture checker on the top face: images/textures in one cue
        def at(u, v):
            ax, ay = T[0][0] + (T[1][0] - T[0][0]) * u, T[0][1] + (T[1][1] - T[0][1]) * u
            bx, by = T[3][0] + (T[2][0] - T[3][0]) * u, T[3][1] + (T[2][1] - T[3][1]) * u
            return (ax + (bx - ax) * v, ay + (by - ay) * v)
        for u, v in ((0, 0), (0.5, 0.5)):
            shapes.append(("poly", [at(u, v), at(u + .5, v), at(u + .5, v + .5), at(u, v + .5)], CREAM))
    return shapes


def layout(size):
    return "tiny" if size <= 20 else "small" if size <= 32 else "full"


def geometry(variant):
    """Shapes on a 1024 canvas. Returns list of (kind, data, color[, width])."""
    g = [("tile", None, None)]
    if variant == "tiny":  # the timeline cannot survive at 16-20 px, keep only a large cube
        g += cube_faces(512, 512, 330, checker=True)
    elif variant == "small":
        g += cube_faces(512, 430, 250, checker=True)
        g += [("line", [(200, 830), (824, 830)], TRACK, 76),
              ("line", [(200, 830), (610, 830)], LEFT, 76),
              ("dot", (610, 830, 82), CREAM)]
    else:
        a, w = 96, 30
        for x, y, sx, sy in ((196, 196, 1, 1), (828, 196, -1, 1), (196, 676, 1, -1), (828, 676, -1, -1)):
            g.append(("line", [(x + sx * a, y), (x, y), (x, y + sy * a)], GUIDE, w))
        g += cube_faces(512, 420, 196, checker=True)
        g += [("line", [(224, 812), (800, 812)], TRACK, 28),
              ("line", [(224, 812), (596, 812)], LEFT, 28),
              ("dot", (596, 812, 44), CREAM)]
    return g


def make_svg():
    p = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">',
         '<defs><linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">'
         f'<stop offset="0" stop-color="{TILE_TOP}"/><stop offset="1" stop-color="{TILE_BOTTOM}"/>'
         '</linearGradient></defs>']
    for kind, data, color, *rest in geometry("full"):
        if kind == "tile":
            p.append(f'<rect x="64" y="64" width="896" height="896" rx="200" fill="url(#tile)" '
                     f'stroke="{TILE_EDGE}" stroke-width="10"/>')
        elif kind == "poly":
            p.append(f'<path d="M{" L".join(f"{x:.1f} {y:.1f}" for x, y in data)} Z" fill="{color}"/>')
        elif kind == "line":
            p.append(f'<path d="M{" L".join(f"{x} {y}" for x, y in data)}" fill="none" stroke="{color}" '
                     f'stroke-width="{rest[0]}" stroke-linecap="round" stroke-linejoin="round"/>')
        elif kind == "dot":
            x, y, r = data
            p.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{color}"/>')
    p.append("</svg>")
    return "\n".join(p) + "\n"


def _rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


def _draw_icon(size):
    ss = 4096 if size >= 64 else 2048
    k = ss / 1024
    image = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    for kind, data, color, *rest in geometry(layout(size)):
        if kind == "tile":
            grad = Image.new("RGBA", (ss, ss))
            gd = ImageDraw.Draw(grad)
            a, b = _rgb(TILE_TOP), _rgb(TILE_BOTTOM)
            for y in range(ss):
                t = y / (ss - 1)
                gd.line([(0, y), (ss, y)], fill=tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(4)))
            box = (64 * k, 64 * k, 960 * k, 960 * k)
            mask = Image.new("L", (ss, ss), 0)
            ImageDraw.Draw(mask).rounded_rectangle(box, radius=200 * k, fill=255)
            image.paste(grad, (0, 0), mask)
            edge = max(10 * k, ss / size)  # keep the edge at least ~1 px after downscale
            draw.rounded_rectangle(box, radius=200 * k, outline=_rgb(TILE_EDGE), width=round(edge))
        elif kind == "poly":
            draw.polygon([(x * k, y * k) for x, y in data], fill=_rgb(color))
        elif kind == "line":
            w = rest[0] * k
            pts = [(x * k, y * k) for x, y in data]
            for p0, p1 in zip(pts, pts[1:]):
                draw.line([p0, p1], fill=_rgb(color), width=round(w))
            for x, y in pts:
                draw.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=_rgb(color))
        elif kind == "dot":
            x, y, r = (v * k for v in data)
            draw.ellipse((x - r, y - r, x + r, y + r), fill=_rgb(color))
    return image.resize((size, size), Image.Resampling.LANCZOS)


def make_icon():
    frames = {n: _draw_icon(n) for n in ICO_SIZES}
    # Pillow would otherwise downscale every layer from the first image; pass each tuned frame.
    frames[256].save(ROOT / "formaview.ico", format="ICO", sizes=[(n, n) for n in ICO_SIZES],
                     append_images=[frames[n] for n in ICO_SIZES if n != 256])


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    (ROOT / "formaview.svg").write_text(make_svg(), encoding="utf-8")
    make_icon()


if __name__ == "__main__":
    main()
