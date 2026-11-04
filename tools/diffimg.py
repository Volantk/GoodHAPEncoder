"""Per-channel error stats between two images: python tools/diffimg.py a.png b.png"""
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from viz import load  # noqa: E402

a, b = load(sys.argv[1]), load(sys.argv[2])
d = (a - b) * 255
for c, n in enumerate("RGB"):
    x = d[..., c]
    print(f"{n}: mean {x.mean():+.3f}  rms {np.sqrt((x * x).mean()):.3f}  max|.| {np.abs(x).max():.2f}  (8-bit units)")
big = (np.abs(d).max(-1) > 4).mean() * 100
print(f"pixels off by more than 4/255: {big:.4f}%")
