"""Validate the approved masters and all packaged category icon layers."""
import hashlib
import json
from pathlib import Path
import unittest
import sys
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "src/host/icons/file-types"
SIZES = (16,20,24,32,40,48,64,128,256)


class FileTypeIcons(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "win32", "Windows icon decoder")
    def test_windows_icon_decoder(self):
        import ctypes as ct
        from ctypes import wintypes as wt
        user32 = ct.WinDLL("user32", use_last_error=True)
        user32.LoadImageW.argtypes = [wt.HINSTANCE, wt.LPCWSTR, wt.UINT, ct.c_int, ct.c_int, wt.UINT]
        user32.LoadImageW.restype = wt.HANDLE
        user32.DestroyIcon.argtypes = [wt.HICON]
        for kind in ("image", "video", "model"):
            for size in SIZES:
                with self.subTest(kind=kind, size=size):
                    handle = user32.LoadImageW(None, str(ROOT / f"formaview-{kind}.ico"), 1, size, size, 0x10)
                    self.assertTrue(handle, ct.get_last_error())
                    self.assertTrue(user32.DestroyIcon(handle))

    def test_layers_and_manifest(self):
        manifest = {item["kind"]: item for item in json.loads((ROOT / "manifest.json").read_text(encoding="utf-8-sig"))}
        for kind in ("image", "video", "model"):
            with self.subTest(kind=kind):
                path = ROOT / f"formaview-{kind}.ico"
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), manifest[kind]["sha256"])
                with Image.open(ROOT / f"formaview-{kind}-master.png") as master:
                    self.assertEqual(master.size, (1254,1254))
                    self.assertEqual(master.mode, "RGBA")
                with Image.open(path) as icon:
                    self.assertEqual(icon.ico.sizes(), {(n,n) for n in SIZES})
                    for size in (*SIZES,512):
                        with Image.open(ROOT / f"formaview-{kind}-{size}.png") as png:
                            self.assertEqual(png.size, (size,size))
                            alpha = png.getchannel("A")
                            self.assertGreater(alpha.getextrema()[1], 240)
                            for corner in ((0,0),(size-1,0),(0,size-1),(size-1,size-1)):
                                self.assertEqual(alpha.getpixel(corner), 0)
                            if size <= 256:
                                self.assertEqual(icon.ico.getimage((size,size)).convert("RGBA").tobytes(), png.convert("RGBA").tobytes())


if __name__ == "__main__":
    unittest.main()
