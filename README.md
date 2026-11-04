# GoodHAP

A Hap Q / Hap Q Alpha encoder that keeps gradients and fine detail.

FFmpeg's Hap Q encoder (the one most tools use) bands badly in skies, glows and dark gradients, often with green and pink tints. GoodHAP writes standard Hap Q files (`HapY` / `HapM` QuickTime .mov) that any Hap player reads: Pixera, Resolume, TouchDesigner, VDMX, the Unreal/Unity Hap plugins. It just searches much harder for the best way to store each 4x4 block, so the picture comes out much closer to the source.

- **macOS:** command-line tool (tested on Apple Silicon; Intel Macs should build the same way)
- **Windows:** drag-and-drop app with a side-by-side preview, plus the same command-line tool

## Install on macOS

You need Terminal (in Applications → Utilities) and about 10 minutes the first time.

**1. Install Homebrew** if you don't have it. Paste this into Terminal and follow what it says (it asks for your Mac password and may install Apple's command line tools):

```
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

When it finishes it prints two or three lines under "Next steps" to add Homebrew to your PATH. Run those too.

**2. Download and build GoodHAP:**

```
cd ~
git clone https://github.com/Volantk/GoodHAPEncoder
cd GoodHAPEncoder
./build-mac.sh
```

The script uses the FFmpeg you already have from Homebrew (version 5 or newer, including `ffmpeg@7` style installs) or installs it, adds CMake and pkg-config if they're missing, then builds `goodhap-cli`. It takes a few minutes the first time.

If the build stops with an FFmpeg error, run `brew upgrade ffmpeg` and then `./build-mac.sh` again.

**3. Make it a command you can run from anywhere** (optional, recommended):

```
ln -sf ~/GoodHAPEncoder/build-mac/bin/goodhap-cli "$(brew --prefix)/bin/goodhap"
```

Check that it works with `goodhap --help`.

**Updating later:**

```
cd ~/GoodHAPEncoder && git pull && ./build-mac.sh
```

## Use

```
goodhap clip.mov
```

writes `clip_HapQ.mov` next to the source (`clip_HapQAlpha.mov` when the source has transparency). Tip: type `goodhap ` (with a space), then drag the file from Finder into the Terminal window to paste its path.

More examples:

```
goodhap --quality high clip.mov               # slower, a little better
goodhap *.mov -o ~/Desktop/hap                # every .mov in this folder, into one output folder
goodhap renders/shot_0001.exr -o shot.mov     # a numbered image sequence: give any one frame
goodhap clip.mov --frames 100                 # quick test: first 100 frames only
```

Inputs: anything FFmpeg reads. That includes ProRes (with alpha), DNxHR, H.264/H.265 and PNG, TIFF, EXR, DPX or TGA sequences. 10- and 16-bit sources keep their extra precision, which helps gradients most.

| option | what it does |
|---|---|
| `-o PATH` | output file (one input) or folder. Default: next to each input |
| `--quality fast\|normal\|high\|max` | how hard to search. Default `normal`. `high` for finals, `max` mostly for stills |
| `--dither 0..1` | fine noise that hides banding in gradients. Default `1`. Use `0` for flat graphics and text |
| `--static-dither` | same noise every frame instead of a new pattern per frame (for loops that must match) |
| `--format q\|qa` | force Hap Q or Hap Q Alpha. Default: Alpha only when the source has an alpha channel |
| `--frames N` | stop after N frames |
| `--chunks N` | Hap chunks per frame, for multithreaded playback. Default: automatic |
| `--no-snappy` | skip the lossless snappy stage: bigger files, slightly less CPU in the player |

Ctrl-C stops and deletes the unfinished file. Audio is not copied.

Encoding is CPU work and slower than FFmpeg: on a 24-thread PC, a 3360x1890 clip runs at about 6 fps on `fast`, 3 fps on `normal` and 1.8 fps on `high`. 1080p is roughly three times faster.

## Results

Single frames, PSNR in dB (higher is better, +6 dB is half the error), decoded the way a GPU decodes Hap Q:

| source | FFmpeg hap_q | GoodHAP normal | low-freq: FFmpeg | low-freq: GoodHAP |
|---|---|---|---|---|
| 16-bit sky gradient | 45.4 | 55.7 (61.2 without dither) | 47.1 | 68.0 |
| 16-bit dark vignette | 46.9 | 56.8 (64.2 without dither) | 47.1 | 67.1 |
| UE particle render, DNxHR 3360x1890 | 42.6 | 46.3 | 48.4 | 66.3 |

"Low-freq" is PSNR after blurring both images (sigma 2.5). It measures banding, tints and blotches, which is what you see on an LED wall.

How it gets there:

- **Full-precision input.** 10/16-bit sources go into the block fit as floats. Luma interpolation in the DXT5 alpha block then gives better than 8-bit gradients.
- **Fits the picture the player shows.** Endpoints are chosen to minimise the error of the RGB the Hap Q shader reconstructs, not the error of the stored YCoCg values. Luma is corrected for the chroma error that the block quantisation leaves.
- **Every chroma scale (1-4)** is tried per block. Scales stay in the range where the Hap shader and FFmpeg's CPU decoder agree exactly.
- **Endpoint search** around a PCA/least-squares start, for both the 5:6-bit chroma and the 8-bit luma/alpha endpoints.
- **Blue-noise dither** (tunable, animated or static) turns the remaining quantisation into fine noise instead of bands. It fades out at 0 and 1, so pure black, white and opaque stay exact.
- **Alpha-weighted colour** for Hap Q Alpha: colour bits go where the pixel is visible.

## Windows app

Run `GoodHAP.exe` and drop videos, image sequences (one frame or the folder) or stills on it. Pick a clip to preview it:

- `1` `2` `3`: Source / GoodHAP / FFmpeg (FFmpeg's built-in Hap Q, for comparison)
- hold `Space`: flip to the source
- `D`: difference view; Exposure slider for shadows
- wheel: zoom around the cursor (pixel-exact), drag: pan, double-click: 100% / fit, `F`: fit
- left/right arrows: step frames

The preview uploads real BC3/BC4 textures and decodes them on the GPU with the Hap Q shader, so it shows what a player shows.

### Build on Windows

VS 2022, CMake 3.24+.

```
# FFmpeg 9 shared (LGPL) into third_party/ffmpeg:
gh release download --repo BtbN/FFmpeg-Builds latest -p "ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

On macOS and Linux CMake uses the system FFmpeg 5+ through pkg-config; `build-mac.sh` does the whole thing on a Mac. The `macOS CLI` GitHub workflow builds and tests every change on Apple Silicon against Homebrew's current FFmpeg, `ffmpeg@7` and `ffmpeg@6`.

## Notes

- **VLC, ffplay and mpv** decode Hap on the CPU through FFmpeg, which truncates, so they show GoodHAP files about 1/255 dark in red and blue. GPU decoders (media servers, the Hap plugins) don't. Judge quality in the real player.
- **File size.** Dither makes blocks unique, so the lossless snappy stage compresses less than with FFmpeg's encoder. On real footage the difference is small (2.5 vs 2.1 MB per frame at 3360x1890); on smooth synthetic gradients it is larger. `--dither 0` gives smaller files. Hap Q is bandwidth-bound by design either way.
- **Black next to bright.** Black pixels that share a 4x4 block with a bright particle can lift a little (FFmpeg's encoder does the same). That's a limit of the block format.

## Developer tools

- `ghap_eval gen sky|dark|ramps|detail out.png`: 16-bit test images
- `ghap_eval sweep <source>` / `ghap_eval try <source> c1,a5,s2,d1 ...`: settings experiments
- `ghap_eval metrics <source> <hap.mov>`: score any Hap Q file (FFmpeg's or ours)
- `GoodHAP.exe --capture out.png [--encode] [--view N] [--diff] [--zoom Z cx cy] files...`: hidden-window render of the app to PNG
- `tools/viz.py`: contrast-stretched crop comparisons

Vendored: Vidvox `hap.c` (BSD), Google snappy 1.2.1 (BSD), Dear ImGui 1.91.9 (MIT).
