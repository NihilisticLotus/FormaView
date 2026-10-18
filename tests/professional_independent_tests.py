"""Reopen C++ writer fixtures using external decoders; all output stays external.

bcdec is supplied by the caller as a separately built DLL, never linked into
FormaView. OpenEXR Python is a separate consumer but shares upstream OpenEXR.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
import OpenEXR
from PIL import Image


def dds_decode(path, lib):
    data = path.read_bytes()
    assert data[:4] == b'DDS ' and data[84:88] == b'DX10'
    h, w = struct.unpack_from('<II', data, 12)
    fmt, dimension, misc, layers, alpha = struct.unpack_from('<5I', data, 128)
    assert dimension == 3 and layers == 1 and misc == 0
    if fmt == 28:
        return np.frombuffer(data[148:], np.uint8).reshape(h, w, 4) / 255, fmt, alpha
    family = {71: 1, 74: 2, 77: 3, 80: 4, 81: 4, 83: 5, 84: 5,
              95: 6, 96: 6, 98: 7, 99: 7}[fmt]
    count = {4: 1, 5: 2, 6: 3}.get(family, 4)
    floats = family in (4, 5, 6)
    func = getattr(lib, 'bcdec_bc6h_float' if family == 6 else
                   f'bcdec_bc{family}_float' if floats else f'bcdec_bc{family}')
    func.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int] + ([ctypes.c_int] if floats else [])
    func.restype = None
    block_bytes = 8 if family in (1, 4) else 16
    image = np.empty((h, w, count), np.float32)
    offset = 148
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            block = ctypes.create_string_buffer(data[offset:offset + block_bytes])
            values = np.zeros((4, 4, count), np.float32 if floats else np.uint8)
            args = [block, values.ctypes.data, 4 * count]
            if floats:
                args.append(int(fmt in (81, 84, 96)))
            func(*args)
            image[by:min(h, by+4), bx:min(w, bx+4)] = values[:min(4, h-by), :min(4, w-bx)] / (1 if floats else 255)
            offset += block_bytes
    assert offset == len(data), 'Unexpected DDS trailing data/subresources'
    return image, fmt, alpha


def rgbe_decode(path):
    data = path.read_bytes()
    end = data.index(b'\n\n') + 2
    row_end = data.index(b'\n', end)
    axis_y, height, axis_x, width = data[end:row_end].split()
    assert axis_y == b'-Y' and axis_x == b'+X'
    height, width = int(height), int(width)
    raw = np.zeros((height, width, 4), dtype=np.uint8)
    pos = row_end + 1
    for y in range(height):
        assert data[pos:pos+4] == bytes([2, 2, width >> 8, width & 255])
        pos += 4
        for c in range(4):
            x = 0
            while x < width:
                count = data[pos]; pos += 1
                if count > 128:
                    count -= 128
                    raw[y, x:x+count, c] = data[pos]; pos += 1
                else:
                    assert count > 0
                    raw[y, x:x+count, c] = list(data[pos:pos+count]); pos += count
                x += count
            assert x == width
    assert pos == len(data)
    # Standard lower-edge interpretation, deliberately independent of the
    # DirectXTex midpoint reconstruction used by the product.
    scale = np.exp2(raw[:, :, 3].astype(float) - 136)
    return raw[:, :, :3] * scale[:, :, None]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--bcdec', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    lib = ctypes.CDLL(str(args.bcdec.resolve()))
    results = []
    for path in sorted(args.fixtures.iterdir()):
        record = {'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        try:
            if path.suffix == '.exr':
                f = OpenEXR.File(str(path), separate_channels=True)
                header = f.header()
                assert list(header['dataWindow'][0]) == [-3, 7]
                assert list(header['dataWindow'][1]) == [4, 14]
                assert list(header['displayWindow'][0]) == [0, 0]
                assert header['name'] == 'actual.beauty'
                expected = {'beauty.R': .25, 'G': .5, 'B': .75, 'A': .5}
                assert set(f.channels()) == set(expected)
                for name, value in expected.items():
                    assert np.array_equal(f.channels()[name].pixels, np.full((8, 8), value, np.float32))
                record['consumer'] = 'OpenEXR Python ' + OpenEXR.__version__ + ' (same upstream codec)'
            elif path.suffix == '.tga':
                a = np.array(Image.open(path))
                expected = [64, 128, 191, 0 if path.stem == 'tga-4' else 128]
                count = 1 if a.ndim == 2 else a.shape[2]
                if path.stem.startswith('tga-'):
                    assert count == int(path.stem[-1])
                assert np.all(a == (expected[0] if count == 1 else expected[:count]))
                record['consumer'] = 'Pillow TGA'
            elif path.suffix == '.hdr':
                a = rgbe_decode(path)
                error = float(np.abs(a - [.25, .5, .75]).max())
                assert error <= 1/256
                record.update(consumer='Independent Python RGBE parser', max_error=error)
            elif path.suffix == '.dds':
                a, fmt, alpha = dds_decode(path, lib)
                count = a.shape[2]
                expected = np.array([.25, .5, .75, .5][:count])
                if path.stem.startswith('negative-'):
                    expected *= -4 if fmt == 96 else -1
                if count < 4:
                    assert alpha == 3, 'Non-alpha format incorrectly tagged with alpha'
                error = float(np.abs(a[:, :, :min(3, count)] - expected[:min(3, count)]).max())
                assert error < .04
                if count == 4:
                    alpha_error = float(np.abs(a[:, :, 3] - .5).max())
                    assert alpha_error <= (.5 if fmt == 71 else .04)
                    record['alpha_error'] = alpha_error
                record.update(consumer='bcdec independent algorithm' if fmt != 28 else 'Independent native DDS parser',
                              dxgi=fmt, alpha_mode=alpha, max_error=error)
            else:
                continue
            record['pass'] = True
        except Exception as exc:
            record.update({'pass': False, 'error': repr(exc)})
        results.append(record)
    report = {'passed': sum(r['pass'] for r in results), 'total': len(results), 'results': results}
    (args.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return int(report['passed'] != report['total'])


if __name__ == '__main__':
    raise SystemExit(main())
