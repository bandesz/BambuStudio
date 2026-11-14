#!/usr/bin/env bash
# Exercises scripts/mac_install_app.sh without copying into /Applications.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/mac_install_app.sh"
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
    DEST="$WORK/Applications"
    ERR="$WORK/stderr.txt"
    mkdir -p "$BIN" "$DEST"
    printf '%s\n' '#!/bin/bash' 'case "$1" in
  -s) printf "%s\n" "${FAKE_UNAME_S:-Darwin}" ;;
  -m) printf "%s\n" "${FAKE_UNAME_M:-arm64}" ;;
esac' >"$BIN/uname"
    chmod +x "$BIN/uname"
    export MAC_BUILD_ROOT="$PROJECT"
    export MAC_INSTALL_DEST="$DEST"
    export PATH="$BIN:/usr/bin:/bin"
    export FAKE_UNAME_S="Darwin"
    export FAKE_UNAME_M="arm64"
}

add_built_app() {
    local app="$PROJECT/build/${FAKE_UNAME_M}/BambuStudio/BambuStudio.app/Contents"
    mkdir -p "$app"
    printf '%s\n' "built-app" >"$app/marker.txt"
}

add_stale_app() {
    local app="$DEST/BambuStudio.app"
    mkdir -p "$app"
    printf '%s\n' "stale" >"$app/stale.txt"
}

run_install() {
    set +e
    bash "$SCRIPT" >"$WORK/stdout.txt" 2>"$ERR"
    STATUS=$?
    set -e
}

echo "=== mise.toml ==="
assert_file_contains "install-app task" "$REPO_ROOT/mise.toml" '[tasks."install-app"]'
assert_file_contains "install-app runs script" "$REPO_ROOT/mise.toml" "bash scripts/mac_install_app.sh"

echo "=== replaces /Applications bundle ==="
new_fixture
add_built_app
add_stale_app
run_install
assert_eq "install exit" "0" "$STATUS"
if [ -f "$DEST/BambuStudio.app/Contents/marker.txt" ] && [ ! -e "$DEST/BambuStudio.app/stale.txt" ]; then
    pass "install replaced bundle"
else
    fail "install replaced bundle"
    find "$DEST" -print
fi
assert_eq "source marker kept" "built-app" "$(cat "$PROJECT/build/arm64/BambuStudio/BambuStudio.app/Contents/marker.txt")"

echo "=== missing built app ==="
new_fixture
run_install
assert_eq "missing exit" "1" "$STATUS"
assert_eq "missing stderr" "error: build/arm64/BambuStudio/BambuStudio.app not found. Run: mise build" "$(cat "$ERR")"
if [ -e "$DEST/BambuStudio.app" ]; then
    fail "missing app wrote a destination"
else
    pass "missing app wrote nothing"
fi

echo "=== non-macOS ==="
new_fixture
add_built_app
export FAKE_UNAME_S="Linux"
run_install
assert_eq "linux exit" "1" "$STATUS"
assert_eq "linux stderr" "error: Bambu Studio installs on macOS only (found Linux)" "$(cat "$ERR")"
if [ -e "$DEST/BambuStudio.app" ]; then
    fail "linux wrote a destination"
else
    pass "linux wrote nothing"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures assertion(s) failed"
    exit 1
fi
echo "all assertions passed"
