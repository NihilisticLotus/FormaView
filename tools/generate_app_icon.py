"""Generate FormaView's app icon: a T-pose mannequin threaded by a Mobius infinity loop.

The mannequin stands for characters and their rigs, the one-sided band for looping
animation and 3D inspection: one strand passes behind the body, the other in front of
the hips. The background is transparent. One geometry source drives the SVG and every
ICO layer; 16-32 px use a bolder band and figure without edge lines.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1] / "src/host/icons"
FRONT_HI, FRONT_LO = "#ffc993", "#ee8443"   # band faces turned toward the viewer
BACK_HI, BACK_LO = "#c8581f", "#93380f"     # band faces turned away (the Mobius "other side")
EDGE = "#7a300e"
FIGURE, OUTLINE = "#fff4e2", "#4f220b"
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

LAYOUTS = {
    # a: lemniscate half-width, b: depth separation of the two strands at the crossing,
    # half: band half-width, y0: band centre, foot/h: mannequin feet and height (1024 canvas)
    "full": dict(a=395, b=95, half=68, y0=553, foot=906, h=820, outline=0.024, edge=3),
    "small": dict(a=400, b=110, half=92, y0=574, foot=934, h=880, outline=0.03, edge=0),
}


def layout(size):
    return "small" if size <= 32 else "full"


def _hex(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def _mix(a, b, t):
    a, b = _hex(a), _hex(b)
    return "#" + "".join(f"{round(a[i] + (b[i] - a[i]) * t):02x}" for i in range(3))


def _sub(p, q):
    return (p[0] - q[0], p[1] - q[1], p[2] - q[2])


def _cross(p, q):
    return (p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2], p[0] * q[1] - p[1] * q[0])


def _unit(p):
    n = math.sqrt(sum(c * c for c in p)) or 1
    return tuple(c / n for c in p)


def band_quads(a, b, half, tilt=20, n=1100):
    """Depth-sorted quads (z, [4 xy points], colour) of a Mobius band on a lemniscate.

    The width direction turns half a revolution around the tangent over one loop, so the
    band is one-sided; the twist sits at the left lobe, where the band turns edge-on and
    its faces swap tone. A continuous normal must flip once more somewhere, so the loop
    starts where the back strand crosses behind the torso, hiding that flip.
    View: x right, y up, z to viewer.
    """
    tilt = math.radians(tilt)

    def centre(t):
        s = 1 + math.sin(t) ** 2
        return (a * math.cos(t) / s, a * math.sin(t) * math.cos(t) / s, b * math.sin(t))

    def view(p):
        x, y, z = p
        return (x, y * math.cos(tilt) - z * math.sin(tilt), y * math.sin(tilt) + z * math.cos(tilt))

    def edges(t):
        e = 1e-4
        tangent = _unit(_sub(centre(t + e), centre(t - e)))
        normal = _unit(_cross((0, 0, 1), tangent))
        d = tuple(math.cos(t / 2) * normal[j] + math.sin(t / 2) * (0, 0, 1)[j] for j in range(3))
        c = centre(t)
        return (view(tuple(c[j] + half * d[j] for j in range(3))),
                view(tuple(c[j] - half * d[j] for j in range(3))))

    quads = []
    for i in range(n):
        (a0, b0), (a1, b1) = edges(math.pi * (1.5 + 2 * i / n)), edges(math.pi * (1.5 + 2 * (i + 1) / n))
        facing = _unit(_cross(_sub(a1, a0), _sub(b0, a0)))[2]
        sheen = min(1, max(0, 0.5 - (a0[0] + b1[0]) / (4 * a) + 0.35 * abs(facing)))
        colour = _mix(FRONT_LO, FRONT_HI, sheen) if facing > 0 else _mix(BACK_LO, BACK_HI, sheen)
        z = (a0[2] + a1[2] + b0[2] + b1[2]) / 4
        quads.append((z, [(p[0], p[1]) for p in (a0, a1, b1, b0)], colour))
    return sorted(quads, key=lambda q: q[0])


def mannequin(foot, h, outline):
    """Shapes for a T-pose mannequin: (kind, data, colour, width) on the 1024 canvas."""
    def P(x, y):
        return (512 + x * h, foot - y * h)
    limbs = [((-0.475, 0.745), (0.475, 0.745), 0.088),   # arms in T-pose
             ((0, 0.80), (0, 0.74), 0.075),              # neck
             ((-0.068, 0.47), (-0.115, 0.035), 0.108),   # legs
             ((0.068, 0.47), (0.115, 0.035), 0.108)]
    torso = [(-0.135, 0.785), (0.135, 0.785), (0.095, 0.5), (0.11, 0.44), (-0.11, 0.44), (-0.095, 0.5)]
    shapes = []
    for colour, grow in ((OUTLINE, outline), (FIGURE, 0)):
        shapes += [("capsule", (P(*p0), P(*p1)), colour, (w + 2 * grow) * h) for p0, p1, w in limbs]
        shapes.append(("poly", [P(*p) for p in torso], colour, 2 * grow * h))
        shapes.append(("circle", (P(0, 0.888), (0.09 + grow) * h), colour, 0))
    return shapes


def geometry(variant, n=1100):
    v = LAYOUTS[variant]
    quads = band_quads(v["a"], v["b"], v["half"], n=n)
    place = lambda q: [(512 + x, v["y0"] - y) for x, y in q]
    band = lambda part: [("quad", place(q), c, v["edge"]) for _, q, c in part]
    return (band([q for q in quads if q[0] < 0]), mannequin(v["foot"], v["h"], v["outline"]),
            band([q for q in quads if q[0] >= 0]))


def _pts(points):
    return " L".join(f"{x:.1f} {y:.1f}" for x, y in points)


def make_svg():
    back, figure, front = geometry("full", n=360)

    def band(shapes):
        out = []
        for _, q, colour, edge in shapes:
            out.append(f'<path d="M{_pts(q)} Z" fill="{colour}" stroke="{colour}" stroke-width="1.2"/>')
            out.append(f'<path d="M{_pts(q[:2])} M{_pts(q[2:])}" stroke="{EDGE}" stroke-width="{edge}"/>')
        return "".join(out)

    def body(shapes):
        out = []
        for kind, data, colour, width in shapes:
            if kind == "capsule":
                out.append(f'<path d="M{_pts(data)}" stroke="{colour}" stroke-width="{width:.1f}"/>')
            elif kind == "poly":
                stroke = f' stroke="{colour}" stroke-width="{width:.1f}"' if width else ""
                out.append(f'<path d="M{_pts(data)} Z" fill="{colour}"{stroke}/>')
            else:
                (x, y), r = data
                out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}" fill="{colour}"/>')
        return "".join(out)

    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">\n'
            f'<g id="mobius-back" stroke-linecap="round">{band(back)}</g>\n'
            f'<g id="t-pose-mannequin" fill="none" stroke-linecap="round" stroke-linejoin="round">{body(figure)}</g>\n'
            f'<g id="mobius-front" stroke-linecap="round">{band(front)}</g>\n'
            '</svg>\n')


def _draw_icon(size):
    ss = 4096 if size >= 64 else 2048
    k = ss / 1024
    image = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    S = lambda pts: [(x * k, y * k) for x, y in pts]
    for group in geometry(layout(size)):
        for kind, data, colour, width in group:
            if kind == "quad":
                poly = S(data)
                draw.polygon(poly, fill=colour, outline=colour)
                if width:
                    w = max(1, round(width * k))
                    draw.line(poly[:2], fill=EDGE, width=w)
                    draw.line(poly[2:], fill=EDGE, width=w)
            elif kind == "capsule":
                (p0, p1), w = S(data), width * k
                draw.line([p0, p1], fill=colour, width=round(w))
                for x, y in (p0, p1):
                    draw.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=colour)
            elif kind == "poly":
                pts = S(data)
                draw.polygon(pts, fill=colour)
                if width:
                    draw.line(pts + [pts[0]], fill=colour, width=round(width * k), joint="curve")
            else:
                (x, y), r = (data[0][0] * k, data[0][1] * k), data[1] * k
                draw.ellipse((x - r, y - r, x + r, y + r), fill=colour)
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
