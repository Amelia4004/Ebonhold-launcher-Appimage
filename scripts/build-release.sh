#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:?Usage: ./scripts/build-release.sh <version>}"

rm -rf build
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DEBONHOLD_VERSION="${VERSION}"

cmake --build build --target appimage

APPIMAGE="build/dist/EbonholdUpdater-${VERSION}-x86_64.AppImage"
if [[ ! -f "${APPIMAGE}" ]]; then
  echo "Expected AppImage was not created: ${APPIMAGE}" >&2
  exit 1
fi

sha256sum "${APPIMAGE}" > "${APPIMAGE}.sha256"

echo "Release artifacts:"
ls -lh "${APPIMAGE}" "${APPIMAGE}.sha256"
