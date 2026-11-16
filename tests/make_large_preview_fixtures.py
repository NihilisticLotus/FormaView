"""Generate disposable large JPEG/TGA fixtures in an explicitly named directory."""
from pathlib import Path
import struct
import sys
from PIL import Image


root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
width, height = 18000, 8000
# A 144 MP source exceeds the former 16384 dimension and retained-pixel limits.
with (root / "large-bottom-right.tga").open("xb") as out:
    out.write(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0,
                          width, height, 24, 16))
    rows = [bytes(v for x in range(width)
                  for v in ((x + y) % 256, y, x % 256)) for y in range(256)]
    for y in range(height):
        out.write(rows[y % 256])
seed = Image.new("RGB", (256, 256))
seed.putdata([(x, y, (x + y) % 256) for y in range(256) for x in range(256)])
image = seed.resize((width, height))
image.save(root / "large.jpg", quality=92)
print("Created", width, "x", height, "fixtures in", root)
