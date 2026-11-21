# Platform plans

The capability data for each platform is in `core/src/platform.cpp` (`profileFor()`).
**Status:** no platform is shipped. See [ROADMAP.md](../ROADMAP.md) for the delivery gates.

## macOS: professional desktop DAW
- **Audio:** CoreAudio through AUHAL.
  - Aggregate devices and pro interfaces (USB, Thunderbolt).
  - Sample rates from 44.1 to 192 kHz; buffers from 32 to 2048 frames.
  - Latency compensation uses the driver's reported latency (`recordingOffsetFrames`).
- **Plugins:**
  - AUv2 and AUv3 through AVAudioUnitComponentManager / AudioComponent.
  - VST3 through the Steinberg VST3 SDK.
  - Plugins run in a sandboxed helper process, so a crashing plugin can't take down the session.
  - Plugin editor windows are native NSViews.
- **Builds:** Universal 2 (Apple Silicon and Intel), notarized. Distributed both as a direct
  download and through the Mac App Store; the sandbox limits plugin hosting there (see ADR-0002).
- **Input:** menu bar, full keyboard shortcuts, trackpad gestures, CoreMIDI.

## Windows: professional desktop DAW
- **Audio:**
  - ASIO is preferred for pro interfaces.
  - WASAPI exclusive mode for low latency without ASIO; WASAPI shared as the fallback.
  - Handles device hot-plug and sample-rate changes.
- **Plugins:** VST3 only. AU doesn't exist on Windows, so the compatibility report says so.
  Plugins run in a sandboxed helper process, the same as on macOS.
- **Builds:** x64 first, ARM64 second. ARM64 hosts can't load x64 plugins in-process; the
  out-of-process host is required for that.
- **Input:** mouse and keyboard; pen and touch on touchscreen laptops (Medium/Expanded layouts);
  MIDI through Windows MIDI Services where available, WinMM otherwise.

## iOS / iPadOS: touch-first recording and production
- **Audio:**
  - AVAudioSession (`.playAndRecord`) with RemoteIO.
  - Handles interruptions such as phone calls, route changes (headphones, USB interfaces),
    background audio and Bluetooth latency warnings.
- **Plugins:** AUv3 only, using Apple's AUv3 hosting APIs. Desktop VST3 and AU binaries
  **can't** run. Projects containing them show compatibility warnings with freeze/render options.
- **UI:**
  - iPhone: Compact layout, bottom tabs, "record" workspace first.
  - iPad: Expanded layout with a sidebar, Apple Pencil for note editing and automation drawing,
    and pointer and keyboard support (targets shrink to 24 pt when a trackpad is attached).
- **Limits:** a lower recommended track count (48), adjustable as each device is profiled.

## Android: touch-first recording and production
- **Audio:**
  - AAudio through Oboe, in low-latency exclusive mode where the device supports it, with
    OpenSL ES as the fallback.
  - USB class-compliant interfaces.
  - The latency of each device model is **measured**, not assumed (see the test matrix).
- **Plugins:**
  - Built-in Ynot processors are the supported baseline.
  - Android has no VST3/AU equivalent. AAP (Audio Plugins for Android) is behind a feature flag
    while it is evaluated.
  - Desktop plugins are never assumed to be available.
- **UI:** Compact layout on phones, Expanded on tablets, 48 dp touch targets, Material-adaptive
  navigation (bottom bar, rail, drawer) with Ynot design tokens.
- **Limits:** a lower recommended track count (32) because device performance varies widely.

## Shared across all four
- The same project format, engine, built-in instruments and effects, compatibility rules,
  collaboration protocol and entitlement logic, all from `ynot_core`.
- The same design tokens and layout decisions.
