# Downloading test builds

Every push to the development branch builds Ynot on GitHub and attaches downloadable apps to
that CI run. They are **test builds**, not releases:
- not code-signed or notarized;
- kept for 30 days;
- not "supported" on any platform yet (see ROADMAP.md).

## Easiest on a Mac: build and install in one step
If you've cloned the repository (see TESTING_MACOS.md), this updates Ynot, builds it, runs all
checks, installs it into Applications and opens it. It doesn't need GitHub sign-in, and there's
no unsigned-app warning:
```sh
cd ~/Ynot && git pull && ./tools/install-macos.sh
```

## Where to get them
1. Open the repository's **Actions** tab on GitHub and click the newest green run on the
   `claude/universal-cross-platform-music-mufnfc` branch.
2. Scroll to **Artifacts** at the bottom of the run page and download:
   - **Ynot-macOS-AppleSilicon** for M1/M2/M3/M4 Macs;
   - **Ynot-Windows-x64** for 64-bit Windows 10/11.

   You need to be signed in to GitHub to download artifacts.

## macOS
1. Unzip the download; you may need to unzip twice, because GitHub wraps it in another zip.
   Drag **Ynot.app** to **Applications**.
2. Because the build isn't signed or notarized, macOS blocks it the first time. To open it:
   - **Right-click** (or Control-click) Ynot.app, choose **Open**, then **Open** again in the
     dialog; **or**
   - on macOS 15 and later, open it once, then go to **System Settings, Privacy & Security** and
     click **Open Anyway**; **or**
   - in Terminal: `xattr -dr com.apple.quarantine /Applications/Ynot.app`
3. Allow microphone access when you first record.

## Windows
1. Unzip and run **Ynot.exe**. It's a single file with no installer.
2. SmartScreen may say "Windows protected your PC" because the build isn't signed. Click
   **More info**, then **Run anyway**.
3. Audio uses WASAPI. ASIO needs a build with Steinberg's ASIO SDK (not included).

## Before public distribution
These builds are for testing by the team. Before anyone outside the team downloads Ynot:
- **Signing:** set up Apple Developer ID signing and notarization, and Windows code signing.
- **JUCE license:** settle the JUCE license (ADR-0002). A closed-source release needs a
  commercial JUCE license.
- **ASIO:** if you ship ASIO support, it is subject to Steinberg's licence terms.
