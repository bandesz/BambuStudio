#!/usr/bin/env bash
# Exercises scripts/mac_build.sh without compiling Bambu Studio.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/mac_build.sh"
failures=0

pass() { echo "PASS $*"; }

fail() {
    echo "FAIL $*"
    failures=$((failures + 1))
}

assert_eq() {
    local name="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        pass "$name"
    else
        fail "$name"
        echo "  expected: [$expected]"
        echo "  actual:   [$actual]"
    fi
}

assert_file_contains() {
    local name="$1" file="$2" text="$3"
    if [ ! -f "$file" ]; then
        fail "$name (missing $file)"
        return
    fi
    if grep -F -q -- "$text" "$file"; then
        pass "$name"
    else
        fail "$name"
        echo "  missing text: [$text]"
    fi
}

new_fixture() {
    WORK="$(mktemp -d)"
    BIN="$WORK/bin"
    PROJECT="$WORK/project"
    LOG="$WORK/build-mac.log"
    ERR="$WORK/stderr.txt"
    mkdir -p "$BIN" "$PROJECT"
    cat >"$PROJECT/BuildMac.sh" <<'EOF'
#!/bin/bash
printf '%s\n' "$*" >> "${MAC_BUILD_LOG:?}"
exit 0
EOF
    chmod +x "$PROJECT/BuildMac.sh"
    : >"$LOG"

    write_bin uname '#!/bin/bash
case "$1" in
  -s) printf "%s\n" "${FAKE_UNAME_S:-Darwin}" ;;
  -m) printf "%s\n" "${FAKE_UNAME_M:-arm64}" ;;
esac'
    write_bin cmake '#!/bin/bash
printf "cmake version %s\n" "${FAKE_CMAKE_VERSION:-3.31.10}"
printf "CMake suite\n"'
    write_bin xcode-select '#!/bin/bash
if [ "${FAKE_XCODE_FAIL:-0}" = 1 ]; then
  exit 1
fi
printf "%s\n" /Applications/Xcode.app/Contents/Developer'

    export MAC_BUILD_ROOT="$PROJECT"
    export MAC_BUILD_LOG="$LOG"
    export PATH="$BIN:/usr/bin:/bin"
    export FAKE_UNAME_S="Darwin"
    export FAKE_UNAME_M="arm64"
    export FAKE_CMAKE_VERSION="3.31.10"
    unset FAKE_XCODE_FAIL
}

write_bin() {
    local name="$1" body="$2"
    printf '%s\n' "$body" >"$BIN/$name"
    chmod +x "$BIN/$name"
}

add_dep_tools() {
    local tool
    for tool in automake makeinfo nasm yasm x264 msgfmt; do
        write_bin "$tool" '#!/bin/bash
exit 0'
    done
}

add_sentinel() {
    mkdir -p "$PROJECT/deps/build/${FAKE_UNAME_M}/BambuStudio_deps/usr/local"
}

run_build() {
    set +e
    bash "$SCRIPT" >"$WORK/stdout.txt" 2>"$ERR"
    STATUS=$?
    set -e
}

log_text() { cat "$LOG"; }
err_text() { cat "$ERR"; }

echo "=== mise.toml ==="
assert_file_contains "auto_install enabled" "$REPO_ROOT/mise.toml" "auto_install = true"
assert_file_contains "cmake 3.31" "$REPO_ROOT/mise.toml" 'cmake = "3.31"'
assert_file_contains "build task runs mac_build.sh" "$REPO_ROOT/mise.toml" "bash scripts/mac_build.sh"
if grep -F -q 'ninja = "latest"' "$REPO_ROOT/mise.toml" 2>/dev/null; then
    fail "ninja is not a build tool"
else
    pass "ninja is not a build tool"
fi
if grep -q '^\[tasks.build\]' "$REPO_ROOT/mise.toml" 2>/dev/null; then
    pass "tasks.build table"
else
    fail "tasks.build table"
fi

echo "=== sentinel present skips deps ==="
new_fixture
add_sentinel
# texinfo (makeinfo) is absent on purpose: slicer rebuild must not require dep tools
run_build
assert_eq "skip-deps exit" "0" "$STATUS"
assert_eq "skip-deps log" "-s -t 12.0" "$(log_text)"

echo "=== missing sentinel builds deps then slicer ==="
new_fixture
add_dep_tools
run_build
assert_eq "full-build exit" "0" "$STATUS"
assert_eq "full-build log" $'-d -t 12.0\n-s -t 12.0' "$(log_text)"

echo "=== missing brew tools ==="
new_fixture
add_dep_tools
rm -f "$BIN/makeinfo" "$BIN/nasm"
run_build
assert_eq "missing-tools exit" "1" "$STATUS"
assert_eq "missing-tools log" "" "$(log_text)"
assert_eq "missing-tools stderr" $'error: missing tools: texinfo nasm\nInstall them with: brew install texinfo nasm' "$(err_text)"

echo "=== non-macOS ==="
new_fixture
export FAKE_UNAME_S="Linux"
run_build
assert_eq "linux exit" "1" "$STATUS"
assert_eq "linux log" "" "$(log_text)"
assert_eq "linux stderr" "error: Bambu Studio builds on macOS only (found Linux)" "$(err_text)"

echo "=== cmake version ==="
new_fixture
export FAKE_CMAKE_VERSION="4.0.0"
run_build
assert_eq "cmake exit" "1" "$STATUS"
assert_eq "cmake log" "" "$(log_text)"
assert_eq "cmake stderr" "error: cmake 3.31 is required (found cmake version 4.0.0). Run: mise install" "$(err_text)"

echo "=== stale SDK paths ==="
new_fixture
add_sentinel
mkdir -p "$PROJECT/build/arm64" "$PROJECT/real-deps/usr/local/lib" "$WORK/sdks/MacOSX.sdk" "$WORK/sdks/MacOSX27.2.sdk"
rm -rf "$PROJECT/deps/build/arm64/BambuStudio_deps"
ln -s "$PROJECT/real-deps" "$PROJECT/deps/build/arm64/BambuStudio_deps"
printf '%s\n' \
    'CMAKE_OSX_SYSROOT:PATH='"$WORK/sdks/MacOSX27.0.sdk" \
    'KEEP:PATH='"$WORK/sdks/MacOSX27.2.sdk/usr/lib/libz.tbd" \
    >"$PROJECT/build/arm64/CMakeCache.txt"
printf '%s\n' \
    'old /SDKs/MacOSX27.0.sdk/usr/lib/libz.tbd' \
    >"$PROJECT/real-deps/usr/local/lib/libpng16.cmake"
export MAC_SDK_ROOT="$WORK/sdks"
run_build
assert_eq "stale-sdk exit" "0" "$STATUS"
assert_eq "stale-sdk cache" $'CMAKE_OSX_SYSROOT:PATH='"$WORK/sdks/MacOSX.sdk"$'\nKEEP:PATH='"$WORK/sdks/MacOSX27.2.sdk/usr/lib/libz.tbd" "$(cat "$PROJECT/build/arm64/CMakeCache.txt")"
assert_eq "stale-sdk deps" "old /SDKs/MacOSX.sdk/usr/lib/libz.tbd" "$(cat "$PROJECT/deps/build/arm64/BambuStudio_deps/usr/local/lib/libpng16.cmake")"
unset MAC_SDK_ROOT

echo "=== missing Xcode ==="
new_fixture
export FAKE_XCODE_FAIL="1"
add_sentinel
run_build
assert_eq "xcode exit" "1" "$STATUS"
assert_eq "xcode log" "" "$(log_text)"
assert_eq "xcode stderr" "error: Xcode command line tools not found. Install Xcode from the App Store, then run: xcode-select --install" "$(err_text)"

if [ "$failures" -ne 0 ]; then
    echo "$failures assertion(s) failed"
    exit 1
fi
echo "all assertions passed"
