# GoodHAP

A Hap Q / Hap Q Alpha encoder that keeps gradients and fine detail.

FFmpeg's Hap Q encoder bands badly in skies, glows and dark gradients, often with green/pink tints. GoodHAP writes standard `HapY` / `HapM` QuickTime files that any Hap player reads (Pixera, Resolume, TouchDesigner, VDMX, the Hap plugins), but searches much harder for each 4x4 block:

- **Full-precision input.** 10/16-bit sources (ProRes, DNxHR, PNG/TIFF/EXR sequences) go into the block fit as floats. Luma interpolation in the DXT5 alpha block then gives better than 8-bit gradients.
- **Fits the picture the player shows.** Endpoints are chosen to minimise the error of the RGB the Hap Q shader reconstructs, not the error of the stored YCoCg values. Luma is corrected for the chroma error that the block quantisation leaves.
- **Every chroma scale (1-4)** is tried per block. Scales stay in the range where the Hap shader and FFmpeg's CPU decoder agree exactly.
- **Endpoint search** around a PCA/least-squares start, for both the 5:6-bit chroma and the 8-bit luma/alpha endpoints.
- **Blue-noise dither** (tunable, animated or static) turns the remaining quantisation into fine noise instead of bands. It fades out at 0 and 1, so pure black, white and opaque stay exact.
- **Alpha-weighted colour** for Hap Q Alpha: colour bits go where the pixel is visible.

## Results (single frames, PSNR in dB, decoded like a GPU)

| source | FFmpeg hap_q | GoodHAP normal | low-freq: FFmpeg | low-freq: GoodHAP |
|---|---|---|---|---|
| 16-bit sky gradient | 45.4 | 55.7 (61.2 without dither) | 47.1 | 68.0 |
| 16-bit dark vignette | 46.9 | 56.8 (64.2 without dither) | 47.1 | 67.1 |
| UE particle render, DNxHR 3360x1890 | 42.6 | 46.3 | 48.4 | 66.3 |

"Low-freq" is PSNR after blurring both images (sigma 2.5). It measures banding, tints and blotches, which is what you see on an LED wall.

Speed (24 threads, 3360x1890): Fast ~6 fps, Normal ~3 fps, High ~1.8 fps. FFmpeg's encoder is much faster (~70 fps) and much worse.

## Use

**App:** run `GoodHAP.exe`, drop videos, image sequences (drop one frame or the folder) or stills. Pick a clip to preview it:

- `1` `2` `3`: Source / GoodHAP / FFmpeg (FFmpeg's built-in Hap Q, for comparison)
- hold `Space`: flip to the source
- `D`: difference view; Exposure slider for shadows
- wheel: zoom around the cursor (pixel-exact), drag: pan, double-click: 100% / fit, `F`: fit
- left/right arrows: step frames

The preview uploads real BC3/BC4 textures and decodes them on the GPU with the Hap Q shader, so it shows what a player shows.

**CLI** (Windows and macOS):

```
goodhap-cli [options] <input>... [-o <output.mov | folder>]

goodhap-cli clip.mov                          # -> clip_HapQ.mov next to it
goodhap-cli --quality high *.mov -o ~/hap     # batch into a folder
goodhap-cli renders/shot_0001.exr -o shot.mov # whole numbered sequence
```

Options: `--quality fast|normal|high|max` (default normal), `--dither 0..1`, `--static-dither`, `--format q|qa` (default: Alpha when the source has alpha), `--frames N`, `--chunks N`, `--no-snappy`. Ctrl-C stops and deletes the unfinished file.

## macOS

CLI only. Needs [Homebrew](https://brew.sh); the script installs FFmpeg 8+, CMake and pkg-config if missing.

```
git clone https://github.com/Volantk/GoodHAPEncoder && cd GoodHAPEncoder
./build-mac.sh
./build-mac/bin/goodhap-cli --help
```

The `macOS CLI` GitHub workflow builds and tests it on Apple Silicon and keeps the binary as an artifact. That binary links Homebrew's FFmpeg, so it still needs `brew install ffmpeg` on the machine that runs it.

## Build (Windows)

VS 2022, CMake 3.24+.

```
# FFmpeg 9 shared (LGPL) into third_party/ffmpeg:
gh release download --repo BtbN/FFmpeg-Builds latest -p "ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Vendored: Vidvox `hap.c` (BSD), Google snappy 1.2.1 (BSD), Dear ImGui 1.91.9 (MIT).

## Tools

- `ghap_eval gen sky|dark|ramps|detail out.png`: 16-bit test images
- `ghap_eval sweep <source>` / `ghap_eval try <source> c1,a5,s2,d1 ...`: settings experiments
- `ghap_eval metrics <source> <hap.mov>`: score any Hap Q file (FFmpeg's or ours)
- `GoodHAP.exe --capture out.png [--encode] [--view N] [--diff] [--zoom Z cx cy] files...`: hidden-window render of the app to PNG
- `tools/viz.py`: contrast-stretched crop comparisons

## Notes

- FFmpeg's own CPU Hap decoder (ffplay, VLC, mpv) truncates, so it shows GoodHAP files about 1/255 dark in red and blue. GPU decoders don't.
- Dither makes blocks unique, so snappy compresses less: files are ~70% of uncompressed Hap Q instead of FFmpeg's ~20-50% on synthetic gradients. On real footage the difference is small (2.5 vs 2.1 MB/frame at 3360x1890). Hap Q is bandwidth-bound by design either way.
- Black pixels that share a 4x4 block with a bright particle can lift a little (same as FFmpeg). That's a limit of the block format.
