#!/usr/bin/env bash
# Builds Ynot on a Mac, runs every automated test, and writes a report to paste back.
#
#   cd ~/Ynot && ./tools/macos-test.sh
#
# It only builds inside ./build-macos and writes ./ynot-macos-report.txt; it installs nothing.
# Needs: Xcode or the Command Line Tools (xcode-select --install) and CMake 3.22+
# (brew install cmake). The first run downloads JUCE 8.0.4 (~100 MB) and takes ~5-10 minutes.
set -u
cd "$(dirname "$0")/.."
REPORT="$PWD/ynot-macos-report.txt"
BUILD="$PWD/build-macos"
: > "$REPORT"

log() { echo "$@" | tee -a "$REPORT"; }
step() { log ""; log "== $1"; }
result() {  # name, exit code
    if [ "$2" -eq 0 ]; then log "PASS  $1"; else log "FAIL  $1 (exit $2)"; FAILED=1; fi
}
FAILED=0

step "System"
log "macOS:   $(sw_vers -productVersion 2>/dev/null) ($(sw_vers -buildVersion 2>/dev/null))"
log "Chip:    $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / $(uname -m)"
log "Xcode:   $(xcodebuild -version 2>/dev/null | head -1 || echo 'not found (Command Line Tools only?)')"
log "Clang:   $(clang --version 2>/dev/null | head -1)"
log "CMake:   $(cmake --version 2>/dev/null | head -1 || echo 'NOT FOUND - install with: brew install cmake')"
log "Commit:  $(git rev-parse --short HEAD 2>/dev/null)"
log "Audio:   $(system_profiler SPAudioDataType 2>/dev/null | grep -E '^        [^ ].*:$' | sed 's/^ *//;s/:$//' | paste -sd ',' -)"
command -v cmake >/dev/null || { log "CMake is required: brew install cmake"; exit 1; }

step "Configure (downloads JUCE on first run)"
cmake -S . -B "$BUILD" -DYNOT_BUILD_APP=ON -DCMAKE_BUILD_TYPE=Release >>"$REPORT" 2>&1
result "configure" $?

step "Build (core, CLI, app, tests)"
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >"$BUILD.log" 2>&1
code=$?
grep -E "error|warning: " "$BUILD.log" | grep -v "/_deps/\|/juce" | head -40 >>"$REPORT"
result "build" $code
[ $code -ne 0 ] && { tail -60 "$BUILD.log" >>"$REPORT"; log "Build failed: paste $REPORT to Claude."; exit 1; }

step "Core unit tests"
"$BUILD/ynot_core_tests" >>"$REPORT" 2>&1
result "core tests" $?

step "App adapter tests (JUCE <-> core)"
APPTESTS=$(find "$BUILD" -name YnotAppTests -type f -perm -u+x | head -1)
"$APPTESTS" >>"$REPORT" 2>&1
result "app tests" $?

step "App smoke test (headless; uses your real CoreAudio devices)"
APP=$(find "$BUILD" -path "*Ynot.app/Contents/MacOS/Ynot" -type f | head -1)
"$APP" --smoke-test "$BUILD/smoke.ynot" >>"$REPORT" 2>&1
result "smoke test" $?

step "Cross-device check of a demo project"
"$BUILD/ynot" demo "$BUILD/demo.ynot" >>"$REPORT" 2>&1
"$BUILD/ynot" check "$BUILD/demo.ynot" macos ios >>"$REPORT" 2>&1 || true

log ""
if [ $FAILED -eq 0 ]; then
    log "ALL AUTOMATED CHECKS PASSED."
    log "Next: the hands-on checklist in docs/TESTING_MACOS.md. Launch the app with:"
    log "  open \"${APP%/Contents/MacOS/Ynot}\""
else
    log "SOME CHECKS FAILED."
fi
log "Report saved to: $REPORT  (paste its contents to Claude)"
