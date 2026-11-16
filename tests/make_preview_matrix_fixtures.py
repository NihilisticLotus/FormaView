"""Independent Pillow/struct fixtures for native preview regression.

Usage: python make_preview_matrix_fixtures.py NEW_DIRECTORY [--large]
Only writes a fresh directory; source images are never modified.
"""
import argparse
import binascii
import struct
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageCms


def png16(path, values, interlaced):
    h, w, channels = values.shape
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", binascii.crc32(kind + payload))
    passes = [(0, 0, 1, 1)] if not interlaced else [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    raw = bytearray()
    for x, y, dx, dy in passes:
        for row in values[y::dy, x::dx]:
            if row.size:
                raw += b"\0" + row.astype(">u2").tobytes()
    colour = {1: 0, 2: 4, 3: 2, 4: 6}[channels]
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, colour, 0, 0, int(interlaced))) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--large", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    w, h = (4096, 2304) if args.large else (67, 43)
    index = ((np.arange(h, dtype=np.uint16)[:, None] * 7 + np.arange(w, dtype=np.uint16)[None, :] * 3) % 256).astype(np.uint8)
    pal = Image.fromarray(index).convert("P")
    pal.putpalette([v for n in range(256) for v in (n, (n * 17) % 256, 255 - n)])
    rgb = pal.convert("RGB")
    if args.large:
        second = pal.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        pal.save(args.output / "large-animation.gif", save_all=True, append_images=[second], duration=[70, 130], loop=2, disposal=[2, 3], interlace=True)
        rgb.save(args.output / "large-rle.tga", compression="tga_rle")
        return
    rgba = rgb.convert("RGBA")
    rgba.putalpha(Image.fromarray(index))
    for mode, image in [("rgb", rgb), ("rgba", rgba), ("gray", Image.fromarray(index)), ("gray-alpha", rgba.convert("LA")), ("palette", pal)]:
        image.save(args.output / f"{mode}.png")
    rgba.save(args.output / "icc.png", icc_profile=ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes())
    for name, kind, payload in [("linear", b"gAMA", struct.pack(">I", 100000)), ("srgb", b"sRGB", bytes([0]))]:
        encoded = (args.output / "rgb.png").read_bytes()
        chunk = struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", binascii.crc32(kind + payload))
        (args.output / f"{name}.png").write_bytes(encoded[:33] + chunk + encoded[33:])
    pal.save(args.output / "palette-transparent.png", transparency=4)
    for count in range(1, 5):
        values = np.stack([(index.astype(np.uint32) * 257 + n * 31) % 65536 for n in range(count)], axis=2).astype(np.uint16)
        for interlace in (False, True):
            png16(args.output / f"native16-{count}-{interlace}.png", values, interlace)
    for mode in ("RGB", "RGBA", "P", "1"):
        image = rgba.convert(mode)
        image.save(args.output / f"bmp-{mode}.bmp")
    bmp = (args.output / "bmp-RGB.bmp").read_bytes()
    (args.output / "bare.dib").write_bytes(bmp[14:])
    dds_header = [124, 0x100f, h, w, w * 3, 0, 1] + [0] * 11 + [32, 0x40, 0, 24, 0xff0000, 0xff00, 0xff, 0] + [0x1000, 0, 0, 0, 0]
    (args.output / "legacy24.dds").write_bytes(b"DDS " + struct.pack("<31I", *dds_header) + np.asarray(rgb)[:, :, ::-1].tobytes())
    for name, flags, bits, masks in [("palette8", 0x20, 8, [0, 0, 0, 0]), ("palette-alpha", 0x21, 16, [0, 0, 0, 0xff00]), ("rgbx", 0x40, 32, [0xff, 0xff00, 0xff0000, 0])]:
        header = [124, 0x100f, h, w, w * (bits // 8), 0, 1] + [0] * 11 + [32, flags, 0, bits] + masks + [0x1000, 0, 0, 0, 0]
        if flags & 0x20:
            table = bytes(v for n in range(256) for v in (n, n * 17 % 256, 255 - n, 0))
            payload = index.tobytes() if bits == 8 else np.stack([index, 255 - index], axis=2).tobytes()
        else:
            table = b""
            payload = np.concatenate([np.asarray(rgb), np.zeros((h, w, 1), dtype=np.uint8)], axis=2).tobytes()
        (args.output / f"legacy-{name}.dds").write_bytes(b"DDS " + struct.pack("<31I", *header) + table + payload)
    rgb.save(args.output / "jpeg.jpg", quality=91)
    for alias in ("jpe", "jfif", "jpeg"):
        (args.output / f"jpeg.{alias}").write_bytes((args.output / "jpeg.jpg").read_bytes())
    rgb.save(args.output / "page.tiff")
    (args.output / "page.tif").write_bytes((args.output / "page.tiff").read_bytes())
    for mode in ("RGB", "RGBA", "L", "P"):
        rgba.convert(mode).save(args.output / f"rle-{mode}.tga", compression="tga_rle")
    rgba.save(args.output / "multiple.ico", sizes=[(16, 16), (32, 32), (48, 48)])
    rgba.save(args.output / "dib.ico", sizes=[(16, 16), (32, 32)], bitmap_format="bmp")
    (args.output / "exposure.hdr").write_bytes(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=2.5\n\n-Y 9 +X 11\n" + bytes(v for i in range(99) for v in ((i * 3) % 256, (i * 7) % 256, (i * 11) % 256, 128 + i % 4)))
    (args.output / "exposure.rgbe").write_bytes((args.output / "exposure.hdr").read_bytes())
    # OS/2 core header: RGB rows and no palette.
    stride = ((w * 3 + 3) // 4) * 4
    pixels = b"".join(np.asarray(rgb)[y, :, ::-1].tobytes() + bytes(stride - w * 3) for y in reversed(range(h)))
    core = struct.pack("<IHHHH", 12, w, h, 1, 24)
    (args.output / "core.bmp").write_bytes(struct.pack("<2sIHHI", b"BM", 26 + len(pixels), 0, 0, 26) + core + pixels)
    for green_bits in (5, 6):
        pixels = ((index.astype(np.uint16) % 32) << (green_bits + 5)) | ((index.astype(np.uint16) % (1 << green_bits)) << 5) | ((index.astype(np.uint16) * 3) % 32)
        stride16 = ((w * 2 + 3) // 4) * 4
        payload = b"".join(row.astype("<u2").tobytes() + bytes(stride16 - w * 2) for row in pixels[::-1])
        masks = struct.pack("<III", 31 << (green_bits + 5), ((1 << green_bits) - 1) << 5, 31)
        header = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 16, 3, len(payload), 0, 0, 0, 0)
        offset = 14 + len(header) + len(masks)
        (args.output / f"native16-{green_bits}.bmp").write_bytes(struct.pack("<2sIHHI", b"BM", offset + len(payload), 0, 0, offset) + header + masks + payload)
    # RLE4/RLE8: absolute packets, encoded runs, EOL, delta and EOB.
    for bits in (4, 8):
        palette = bytes(v for n in range(1 << bits) for v in (n * 13 % 256, n * 5 % 256, n * 7 % 256, 0))
        stream = bytes([4, 0x12 if bits == 4 else 2, 0, 0, 0, 2, 1, 0, 3, 0x34 if bits == 4 else 3, 0, 0, 0, 4])
        stream += bytes([0x12, 0x34]) if bits == 4 else bytes([1, 2, 3, 4])
        stream += bytes([0, 0, 0, 1])
        header = struct.pack("<IiiHHIIiiII", 40, 4, 3, 1, bits, 2 if bits == 4 else 1, len(stream), 0, 0, 1 << bits, 0)
        offset = 14 + len(header) + len(palette)
        (args.output / f"rle{bits}.bmp").write_bytes(struct.pack("<2sIHHI", b"BM", offset + len(stream), 0, 0, offset) + header + palette + stream)
    # Stress GIF dictionary growth/reset and all disposal paths, independently
    # decoded by Pillow in the companion reference verification.
    frames = [pal, pal.transpose(Image.Transpose.FLIP_LEFT_RIGHT), pal.transpose(Image.Transpose.FLIP_TOP_BOTTOM)]
    pal.save(args.output / "animation.gif", save_all=True, append_images=frames[1:], duration=[40, 90, 150], loop=1, disposal=[1, 2, 3], interlace=True)


if __name__ == "__main__":
    main()
