"""Max/mean difference between two 16-bit PNGs, in 8-bit units: python tools/cmpdump.py a.png b.png"""
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from viz import load  # noqa: E402

a, b = load(sys.argv[1]), load(sys.argv[2])
if a.shape != b.shape:
    print(f"SIZE {a.shape} vs {b.shape}")
    sys.exit(1)
d = np.abs(a - b) * 255
print(f"max {d.max():6.3f}  mean {d.mean():.4f}  (8-bit units)")
