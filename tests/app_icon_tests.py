"""Brand asset geometry regression. Run with the icon generator's Pillow environment."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / 'src/host/icons'


class BrandAlignment(unittest.TestCase):
    def test_svg_mark_centred_in_frame(self):
        svg = ET.parse(ROOT / 'formaview.svg').getroot()
        group = svg.find('{http://www.w3.org/2000/svg}g')
        self.assertIsNotNone(group)
        dx, dy = map(float, re.findall(r'-?\d+(?:\.\d+)?', group.attrib['transform']))
        points = []
        for path in group:
            values = list(map(float, re.findall(r'-?\d+(?:\.\d+)?', path.attrib['d'])))
            points.extend(zip(values[::2], values[1::2]))
        xs, ys = zip(*points)
        self.assertAlmostEqual((min(xs)+max(xs))/2+dx, 128)
        self.assertAlmostEqual((min(ys)+max(ys))/2+dy, 128)

    def test_all_windows_sizes_keep_mark_centred(self):
        icon = Image.open(ROOT / 'formaview.ico')
        self.addCleanup(icon.close)
        sizes = {16, 20, 24, 32, 40, 48, 64, 128, 256}
        self.assertEqual(icon.ico.sizes(), {(n, n) for n in sizes})
        for n in sorted(sizes):
            with self.subTest(size=n):
                image = icon.ico.getimage((n, n)).convert('RGBA')
                pixels = [(x,y) for y in range(n) for x in range(n)
                          if (lambda c: c[0] > c[1]*1.2 and c[1] > c[2]*1.3 and c[3] > 200)(image.getpixel((x,y)))]
                self.assertTrue(pixels)
                for axis in (0, 1):
                    middle = (min(p[axis] for p in pixels)+max(p[axis] for p in pixels))/2
                    self.assertLessEqual(abs(middle-(n-1)/2), .5)


if __name__ == '__main__':
    unittest.main()
