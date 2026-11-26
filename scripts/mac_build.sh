#!/usr/bin/env bash
# Build Bambu Studio on macOS. Invoked by `mise build` and `mise build-deps`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -n "${MAC_BUILD_ROOT:-}" ]; then
    ROOT="$MAC_BUILD_ROOT"
fi

target="${1:-slicer}"
case "$target" in
    slicer|deps) ;;
    *)
        echo "error: unknown build target: ${target} (expected slicer or deps)" >&2
        exit 1
        ;;
esac

os="$(uname -s)"
if [ "$os" != "Darwin" ]; then
    echo "error: Bambu Studio builds on macOS only (found ${os})" >&2
    exit 1
fi

if ! cmake_version="$(cmake --version 2>/dev/null | head -n 1)"; then
    cmake_version=""
fi
case "$cmake_version" in
    "cmake version 3.31."*) ;;
    "")
        echo "error: cmake 3.31 is required (found none). Run: mise install" >&2
        exit 1
        ;;
    *)
        echo "error: cmake 3.31 is required (found ${cmake_version}). Run: mise install" >&2
        exit 1
        ;;
esac

if ! xcode-select -p >/dev/null 2>&1; then
    echo "error: Xcode command line tools not found. Install Xcode from the App Store, then run: xcode-select --install" >&2
    exit 1
fi

arch="$(uname -m)"

# Xcode point releases remove the previous SDK, but CMake caches and dep
# packages keep absolute paths such as MacOSX27.0.sdk. Point those at the
# unversioned MacOSX.sdk, which Xcode keeps as the current SDK.
rewrite_stale_sdk_paths() {
    local sdkroot="${MAC_SDK_ROOT:-/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs}"
    if [ ! -d "${sdkroot}/MacOSX.sdk" ]; then
        return 0
    fi
    local files=()
    local cache="${ROOT}/build/${arch}/CMakeCache.txt"
    if [ -f "$cache" ]; then
        files+=("$cache")
    fi
    local deps="${ROOT}/deps/build/${arch}/BambuStudio_deps"
    # Trailing slash follows a symlink at the deps root. This install prefix
    # is often a link named BambuStudio_deps.
    if [ -d "$deps" ]; then
        local found
        while IFS= read -r found; do
            [ -n "$found" ] || continue
            files+=("$found")
        done < <(grep -RIl -- 'MacOSX[0-9]' "${deps}/" || true)
    fi
    if [ "${#files[@]}" -eq 0 ]; then
        return 0
    fi
    MAC_SDK_ROOT_FOR_REWRITE="$sdkroot" perl -i -pe '
        my $sdkroot = $ENV{MAC_SDK_ROOT_FOR_REWRITE};
        s{(MacOSX\d[\d.]*\.sdk)}{
            my $name = $1;
            -d "$sdkroot/$name" ? $name : "MacOSX.sdk";
        }ge;
    ' -- "${files[@]}"
}
rewrite_stale_sdk_paths

sentinel="${ROOT}/deps/build/${arch}/BambuStudio_deps/usr/local"
require_dep_tools() {
    local missing=()
    require_tool() {
        local bin="$1" formula="$2"
        if ! command -v "$bin" >/dev/null 2>&1; then
            missing+=("$formula")
        fi
    }
    require_tool automake automake
    require_tool makeinfo texinfo
    require_tool nasm nasm
    require_tool yasm yasm
    require_tool x264 x264
    require_tool msgfmt gettext
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "error: missing tools: ${missing[*]}" >&2
        echo "Install them with: brew install ${missing[*]}" >&2
        exit 1
    fi
}
if [ "$target" = "deps" ]; then
    require_dep_tools
    "$ROOT/BuildMac.sh" -d -t 12.0
    exit 0
fi
if [ ! -d "$sentinel" ]; then
    require_dep_tools
    "$ROOT/BuildMac.sh" -d -t 12.0
fi

"$ROOT/BuildMac.sh" -s -t 12.0
