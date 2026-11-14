"""Brand mark regression. Run with the icon generator's Pillow environment."""
from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET
from PIL import Image

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO / 'src/host/icons'
NS = '{http://www.w3.org/2000/svg}'
sys.path.insert(0, str(REPO / 'tools'))
import generate_app_icon as gen  # noqa: E402


class BrandMark(unittest.TestCase):
    def test_svg_is_the_generated_mannequin_threaded_by_a_mobius_loop(self):
        text = (ROOT / 'formaview.svg').read_text(encoding='utf-8')
        self.assertEqual(text, gen.make_svg())
        svg = ET.fromstring(text)
        self.assertEqual(svg.attrib['viewBox'], '0 0 1024 1024')
        self.assertEqual(svg.findall('.//' + NS + 'rect'), [])  # transparent, no tile
        groups = [g.attrib['id'] for g in svg.findall(NS + 'g')]
        self.assertEqual(groups, ['mobius-back', 't-pose-mannequin', 'mobius-front'])
        figure = svg.find(NS + "g[@id='t-pose-mannequin']")
        self.assertEqual({n.attrib.get('fill') for n in figure.iter(NS + 'circle')}, {gen.OUTLINE, gen.FIGURE})
        for strand in ('mobius-back', 'mobius-front'):
            self.assertTrue(svg.find(f"{NS}g[@id='{strand}']"))

    def test_band_is_one_sided_and_shows_both_tones(self):
        quads = gen.band_quads(395, 95, 68, n=360)
        colours = {c for _, _, c in quads}
        front = {gen._mix(gen.FRONT_LO, gen.FRONT_HI, t / 100) for t in range(101)}
        self.assertTrue(colours & front)
        self.assertTrue(colours - front)  # the twist turns the far side toward the viewer
        self.assertTrue(any(z < 0 for z, _, _ in quads) and any(z > 0 for z, _, _ in quads))

    def test_every_windows_size_is_transparent_and_centered(self):
        icon = Image.open(ROOT / 'formaview.ico')
        self.addCleanup(icon.close)
        self.assertEqual(icon.ico.sizes(), {(n, n) for n in gen.ICO_SIZES})
        for n in gen.ICO_SIZES:
            with self.subTest(size=n):
                image = icon.ico.getimage((n, n)).convert('RGBA')
                self.assertEqual(image.tobytes(), gen._draw_icon(n).tobytes())
                alpha = image.getchannel('A')
                for corner in ((0, 0), (n - 1, 0), (0, n - 1), (n - 1, n - 1)):
                    self.assertEqual(alpha.getpixel(corner), 0)
                bbox = alpha.getbbox()
                self.assertLessEqual(abs((bbox[0] + bbox[2]) / 2 - n / 2), max(1, n * 0.012))
                self.assertLessEqual(abs((bbox[1] + bbox[3]) / 2 - n / 2), max(1, n * 0.012))

    def test_small_sizes_use_the_bolder_layout(self):
        self.assertEqual({gen.layout(n) for n in (16, 20, 24, 32)}, {'small'})
        self.assertEqual({gen.layout(n) for n in (40, 48, 64, 128, 256)}, {'full'})


if __name__ == '__main__':
    unittest.main()
