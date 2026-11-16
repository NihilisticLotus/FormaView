"""Independent Pillow oracle for the optional --preview-samples evidence.

Usage: py -3 tests/large_preview_reference_tests.py SAMPLES EVIDENCE_JSON
No writes to samples. Only CPU region/pixel evidence, not GPU verification.
"""
import hashlib
import json
from pathlib import Path
import sys
from PIL import Image


def main():
    directory, evidence = map(Path, sys.argv[1:])
    report = json.loads(evidence.read_text(encoding="utf-8"))
    for sample in report["samples"]:
        path = directory / sample["file"]
        with path.open("rb") as source:
            assert hashlib.file_digest(source, "sha256").hexdigest() == sample["sha256"]
        with Image.open(path) as image:
            image.load()
            for probe in sample["probes"]:
                expected = image.getpixel((int(probe["x"]), int(probe["y"])))
                if isinstance(expected, int):
                    expected = (expected,)
                actual = tuple(int(c["source"]) for c in probe["source"])
                assert actual == expected, (path.name, probe["x"], probe["y"], actual, expected)
            x, y = image.width // 2, image.height // 2
            rgba = image.crop((x, y, x + 256, y + 192)).convert("RGBA").tobytes()
            assert hashlib.sha256(rgba).hexdigest() == sample["region_rgba8_sha256"], path.name
        assert sample["retained_bytes"] <= 32 * 1024 * 1024
        print("PASS", path.name, "exact source pixels, native ROI, bounded storage")


if __name__ == "__main__":
    main()
