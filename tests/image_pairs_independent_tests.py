"""Validate real MCP cross-format outputs with consumers outside FormaView."""
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True

import numpy as np
import OpenEXR
from PIL import Image
from professional_independent_tests import dds_decode, rgbe_decode


def decode(path):
    suffix = path.suffix.lower()
    if suffix == '.dds':
        # The matrix requests uncompressed native RGBA8; no bcdec DLL needed.
        pixels, _, _ = dds_decode(path, None)
    elif suffix == '.hdr':
        pixels = rgbe_decode(path)
    elif suffix == '.exr':
        channels = OpenEXR.File(str(path), separate_channels=True).channels()
        pixels = np.stack([channels[c].pixels for c in 'RGBA' if c in channels], axis=-1)
    else:
        pixels = np.asarray(Image.open(path).convert('RGBA'), dtype=float) / 255
    if pixels.shape[-1] == 3:
        pixels = np.concatenate([pixels, np.ones((*pixels.shape[:2], 1))], axis=-1)
    assert pixels.shape == (8, 8, 4), (path, pixels.shape)
    assert np.isfinite(pixels).all(), path
    return pixels


def main():
    records = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
    assert len(records) == 99
    results = []
    for record in records:
        expected = decode(Path(record['input']))
        actual = decode(Path(record['output']))
        if record['drop_alpha']:
            expected[:, :, 3] = 1
        rgb_error = float(np.max(np.abs(expected[:, :, :3] - actual[:, :, :3])))
        alpha_error = float(np.max(np.abs(expected[:, :, 3] - actual[:, :, 3])))
        assert rgb_error < .04 and alpha_error <= 1 / 255 + 1e-6, (record, rgb_error, alpha_error)
        results.append(dict(record, rgb_max_error=rgb_error, alpha_max_error=alpha_error))
    Path(sys.argv[2]).write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'PASS {len(results)} independently decoded conversion pairs')


if __name__ == '__main__':
    main()
