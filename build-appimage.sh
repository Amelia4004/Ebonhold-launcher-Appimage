#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target appimage

echo
echo "AppImage output directory: $(pwd)/build/dist"
