"""Build the Windows multi-resolution ICO from the transparent master PNG."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "src/host/icons"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def make_icon():
    source = ROOT / "formaview.png"
    with Image.open(source) as master:
        if master.mode != "RGBA":
            raise ValueError(f"{source} must be RGBA to preserve transparency; got {master.mode}")
        if master.getchannel("A").getextrema()[0] != 0:
            raise ValueError(f"{source} must contain transparent pixels")
        master.save(ROOT / "formaview.ico", sizes=[(size, size) for size in SIZES])


if __name__ == "__main__":
    make_icon()
