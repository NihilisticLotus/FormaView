"""Compare preview compositor output with Pillow's independent GIF decoder."""
import hashlib
import json
import sys

import numpy as np
from PIL import Image

evidence = json.load(open(sys.argv[1], encoding="utf-8"))
for case in evidence["frames"]:
    with Image.open(case["file"]) as image:
        image.seek(case["frame"])
        rgba = np.asarray(image.convert("RGBA"))
        x = case["x"] + np.arange(case["outputWidth"], dtype=np.int64) * case["width"] // case["outputWidth"]
        y = case["y"] + np.arange(case["outputHeight"], dtype=np.int64) * case["height"] // case["outputHeight"]
        expected = rgba[y[:, None], x[None, :], :]
        assert hashlib.sha256(expected.tobytes()).hexdigest() == case["sha256"], case
        assert case["retainedBytes"] <= 80 * 1024 * 1024, case
print(f'Passed {len(evidence["frames"])} independently decoded GIF frame/region comparisons')
