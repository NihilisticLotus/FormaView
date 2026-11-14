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
    def test_svg_is_the_generated_cube_viewfinder_timeline_tile(self):
        text = (ROOT / 'formaview.svg').read_text(encoding='utf-8')
        self.assertEqual(text, gen.make_svg())
        svg = ET.fromstring(text)
        self.assertEqual(svg.attrib['viewBox'], '0 0 1024 1024')
        self.assertEqual(len(svg.findall(NS + 'rect')), 1)
        fills = [node.attrib.get('fill') for node in svg.findall(NS + 'path')]
        for colour in (gen.TOP, gen.LEFT, gen.RIGHT):
            self.assertEqual(fills.count(colour), 1)
        self.assertEqual(fills.count(gen.CREAM), 2)  # texture checker
        strokes = [node.attrib.get('stroke') for node in svg.findall(NS + 'path')]
        self.assertEqual(strokes.count(gen.GUIDE), 4)  # viewfinder corners
        self.assertEqual(len(svg.findall(NS + 'circle')), 1)  # timeline playhead

    def test_every_windows_size_has_its_own_centered_tile(self):
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
                self.assertLessEqual(abs((bbox[0] + bbox[2] - 1) / 2 - (n - 1) / 2), 0.5)
                self.assertLessEqual(abs((bbox[1] + bbox[3] - 1) / 2 - (n - 1) / 2), 0.5)
                self.assertEqual(alpha.getpixel((n // 2, n // 2)), 255)

    def test_small_sizes_use_simplified_layouts(self):
        self.assertEqual({gen.layout(n) for n in (16, 20)}, {'tiny'})
        self.assertEqual({gen.layout(n) for n in (24, 32)}, {'small'})
        self.assertEqual({gen.layout(n) for n in (40, 48, 64, 128, 256)}, {'full'})


if __name__ == '__main__':
    unittest.main()
