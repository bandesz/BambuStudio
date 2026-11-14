#!/usr/bin/env bash
# Copy the macOS app bundle produced by `mise build` to /Applications.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -n "${MAC_BUILD_ROOT:-}" ]; then
    ROOT="$MAC_BUILD_ROOT"
fi
DEST_DIR="${MAC_INSTALL_DEST:-/Applications}"

os="$(uname -s)"
if [ "$os" != "Darwin" ]; then
    echo "error: Bambu Studio installs on macOS only (found ${os})" >&2
    exit 1
fi

arch="$(uname -m)"
src="${ROOT}/build/${arch}/BambuStudio/BambuStudio.app"
if [ ! -d "$src" ]; then
    echo "error: build/${arch}/BambuStudio/BambuStudio.app not found. Run: mise build" >&2
    exit 1
fi

dest="${DEST_DIR}/BambuStudio.app"
rm -rf "$dest"
ditto "$src" "$dest"
