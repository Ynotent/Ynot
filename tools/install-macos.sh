#!/usr/bin/env bash
# One step: update Ynot, build it, run every automated check, install it into
# /Applications and open it.
#
#   cd ~/Ynot && git pull && ./tools/install-macos.sh
#
# The app is built on this Mac, so macOS doesn't show the "unidentified developer" warning.
# Your settings (audio device, scanned plugins) live in ~/Library/Application Support/Ynot
# and are kept.
set -u
cd "$(dirname "$0")/.."

echo "== Updating Ynot"
git pull --ff-only || echo "(couldn't update; installing the version you already have)"

echo "== Building and testing (first time ~10 min, updates ~1-3 min)"
if ! ./tools/macos-test.sh; then
    echo ""
    echo "Something failed, so Ynot was NOT installed. Copy the report and paste it to Claude:"
    echo "  cat ~/Ynot/ynot-macos-report.txt | pbcopy"
    exit 1
fi

APP=$(find build-macos -path "*Release/Ynot.app" -maxdepth 5 -type d | head -1)
if [ -z "$APP" ]; then
    echo "Build finished but Ynot.app wasn't found. Paste this to Claude:"
    ls -R build-macos/app | head -40
    exit 1
fi

echo "== Installing into /Applications"
osascript -e 'tell application "Ynot" to quit' >/dev/null 2>&1 || true
sleep 1
rm -rf /Applications/Ynot.app
if ! cp -R "$APP" /Applications/; then
    echo "Couldn't write to /Applications. Trying with your password:"
    sudo cp -R "$APP" /Applications/ || exit 1
fi
xattr -dr com.apple.quarantine /Applications/Ynot.app >/dev/null 2>&1 || true

echo "== Opening Ynot"
open /Applications/Ynot.app
echo ""
echo "Done. Ynot is installed in Applications (also in Launchpad)."
echo "To update later, run the same command again:  cd ~/Ynot && git pull && ./tools/install-macos.sh"
