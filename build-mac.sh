#!/bin/sh
# Builds goodhap-cli on macOS. Needs Homebrew (https://brew.sh).
# Uses the Homebrew FFmpeg that is already installed (ffmpeg, or ffmpeg@7/@6/@5),
# installs the current one if there is none. GHAP_BREW_FFMPEG=ffmpeg@7 picks one.
set -e
cd "$(dirname "$0")"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is missing. Install it from https://brew.sh, then run this again." >&2
  exit 1
fi

formula="$GHAP_BREW_FFMPEG"
if [ -z "$formula" ]; then
  for f in ffmpeg ffmpeg@8 ffmpeg@7 ffmpeg@6 ffmpeg@5; do
    if brew list --versions "$f" >/dev/null 2>&1; then formula="$f"; break; fi
  done
fi
if [ -z "$formula" ]; then
  formula=ffmpeg
  echo "Installing FFmpeg through Homebrew..."
  brew install ffmpeg
elif ! brew list --versions "$formula" >/dev/null 2>&1; then
  brew install "$formula"
fi
command -v cmake >/dev/null 2>&1 || brew install cmake
command -v pkg-config >/dev/null 2>&1 || brew install pkgconf

prefix="$(brew --prefix "$formula")"
echo "Using $(brew list --versions "$formula") from $prefix"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# A cache from an earlier failed run may still point at another FFmpeg.
rm -f build-mac/CMakeCache.txt
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --parallel --target goodhap-cli ghap_eval

echo
echo "Built: $(pwd)/build-mac/bin/goodhap-cli"
echo "Try:   ./build-mac/bin/goodhap-cli --help"
echo "To use it from anywhere as 'goodhap':"
echo "       ln -sf \"$(pwd)/build-mac/bin/goodhap-cli\" \"$(brew --prefix)/bin/goodhap\""
