#!/bin/sh
# Builds goodhap-cli on macOS. Needs Homebrew (https://brew.sh).
set -e
cd "$(dirname "$0")"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is missing. Install it from https://brew.sh, then run this again." >&2
  exit 1
fi
brew list ffmpeg >/dev/null 2>&1 || brew install ffmpeg
command -v cmake >/dev/null 2>&1 || brew install cmake
command -v pkg-config >/dev/null 2>&1 || brew install pkgconf

cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --parallel --target goodhap-cli ghap_eval

echo
echo "Built: $(pwd)/build-mac/bin/goodhap-cli"
echo "Try:   ./build-mac/bin/goodhap-cli --help"
echo "To use it from anywhere as 'goodhap':"
echo "       ln -sf \"$(pwd)/build-mac/bin/goodhap-cli\" \"$(brew --prefix)/bin/goodhap\""
