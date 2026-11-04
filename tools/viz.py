"""Contrast-stretched crop comparisons for 16-bit PNGs.

python tools/viz.py out.png --crop x y w h --gain 8 --zoom 2 label=path [label=path ...]

The first image is the reference: every crop is shown as
0.5 + (image - mean of the reference crop) * gain, so banding and tints that
are invisible at 1x become obvious.
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

FFMPEG = str(Path(__file__).resolve().parents[1] / "third_party/ffmpeg/bin/ffmpeg.exe")
FFPROBE = FFMPEG.replace("ffmpeg.exe", "ffprobe.exe")


def load(path):
    probe = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                            "-of", "csv=p=0", path], capture_output=True, text=True, check=True).stdout.strip()
    w, h = map(int, probe.split(",")[:2])
    raw = subprocess.run([FFMPEG, "-v", "error", "-i", path, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb48le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint16).reshape(h, w, 3).astype(np.float32) / 65535.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--crop", nargs=4, type=int, required=True)
    ap.add_argument("--gain", type=float, default=8)
    ap.add_argument("--zoom", type=int, default=2)
    ap.add_argument("images", nargs="+")
    a = ap.parse_args()
    x, y, w, h = a.crop
    tiles = []
    ref_mean = None
    for item in a.images:
        label, path = item.split("=", 1)
        img = load(path)[y:y + h, x:x + w]
        if ref_mean is None:
            ref_mean = img.reshape(-1, 3).mean(0)
        v = np.clip(0.5 + (img - ref_mean) * a.gain, 0, 1)
        t = Image.fromarray((v * 255 + 0.5).astype(np.uint8)).resize((w * a.zoom, h * a.zoom), Image.NEAREST)
        d = ImageDraw.Draw(t)
        d.rectangle([0, 0, 8 * len(label) + 8, 16], fill=(0, 0, 0))
        d.text((4, 2), label, fill=(255, 255, 255))
        tiles.append(t)
    out = Image.new("RGB", (tiles[0].width, sum(t.height for t in tiles) + 4 * (len(tiles) - 1)), (40, 40, 40))
    oy = 0
    for t in tiles:
        out.paste(t, (0, oy))
        oy += t.height + 4
    out.save(a.out)


if __name__ == "__main__":
    sys.exit(main())
