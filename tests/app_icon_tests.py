"""Transparent app-icon asset regression tests."""
from pathlib import Path
import unittest
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / 'src/host/icons'
SIZES = {16, 20, 24, 32, 40, 48, 64, 128, 256}


class AppIconTests(unittest.TestCase):
    def test_master_png_is_rgba_with_transparent_background_and_both_brand_colors(self):
        with Image.open(ROOT / 'formaview.png') as source:
            self.assertEqual(source.mode, 'RGBA')
            alpha = source.getchannel('A')
            self.assertEqual(alpha.getextrema()[0], 0)
            self.assertGreater(alpha.getextrema()[1], 240)
            for corner in ((0, 0), (source.width - 1, 0),
                           (0, source.height - 1), (source.width - 1, source.height - 1)):
                self.assertEqual(alpha.getpixel(corner), 0)
            bbox = alpha.getbbox()
            self.assertIsNotNone(bbox)
            self.assertLessEqual(abs((bbox[0] + bbox[2] - 1) / 2 - (source.width - 1) / 2), 4)
            # The approved perspective mark intentionally carries a little
            # more visual mass above its lower front edge.
            self.assertLessEqual(abs((bbox[1] + bbox[3] - 1) / 2 - (source.height - 1) / 2), 16)
            pixels = source.convert('RGB').load()
            has_orange = has_blue = False
            for y in range(0, source.height, 4):
                for x in range(0, source.width, 4):
                    r, g, b = pixels[x, y]
                    has_orange |= r > 170 and g < 150 and b < 150
                    has_blue |= b > 70 and r < 70 and g < 120
            self.assertTrue(has_orange)
            self.assertTrue(has_blue)

    def test_ico_contains_alpha_preserving_windows_sizes(self):
        icon = Image.open(ROOT / 'formaview.ico')
        self.addCleanup(icon.close)
        self.assertEqual(icon.ico.sizes(), {(n, n) for n in SIZES})
        for n in sorted(SIZES):
            with self.subTest(size=n):
                image = icon.ico.getimage((n, n)).convert('RGBA')
                alpha = image.getchannel('A')
                self.assertEqual(alpha.getextrema()[0], 0)
                self.assertGreater(alpha.getextrema()[1], 240)
                for corner in ((0, 0), (n - 1, 0), (0, n - 1), (n - 1, n - 1)):
                    self.assertEqual(alpha.getpixel(corner), 0)
                self.assertGreater(alpha.getpixel((n // 2, n // 2)), 0)


if __name__ == '__main__':
    unittest.main()
