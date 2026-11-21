# ADR-0002: UI toolkit and plugin hosting framework

**Status:** Proposed. **Needs an owner decision** (licensing and budget).

## Context
The shells need four things:
1. audio I/O (CoreAudio, ASIO/WASAPI, AVAudioSession, AAudio);
2. plugin hosting (AU/AUv3/VST3 on desktop, AUv3 on iOS);
3. a GPU-friendly timeline and mixer UI that adapts from phones to multi-monitor desktops;
4. native plugin editor windows.

## Recommendation
**Use JUCE 8 for the app shells on all four platforms, on top of `ynot_core`.**

- It is the only mature toolkit that covers all of the following on all four targets:
  - CoreAudio, ASIO, WASAPI, AVAudioSession and Oboe/AAudio I/O;
  - VST3, AU and AUv3 hosting;
  - a single custom-drawn UI.
- It gives one UI codebase. The UI uses `decideLayout()` and `design/tokens.json` to adapt per
  form factor, which matches the "one ecosystem, consistent identity" requirement directly.
- Native integration where users notice it is added per platform:
  - the macOS menu bar;
  - iOS/Android share sheets, document pickers and haptics;
  - StoreKit and Play Billing.

### Owner decisions required before M1
1. **JUCE license.** JUCE is dual-licensed (AGPLv3 or commercial). A closed-source commercial
   app needs a paid JUCE license. If that's not acceptable, the fallback is below.
2. **Mac App Store distribution.** Sandboxed apps have restrictions on hosting AUv2 and VST3
   plugins. The recommendation is to ship the pro macOS DAW as a direct download (notarized)
   first, and to treat a Mac App Store build as an optional later edition limited to AUv3.

### Fallback (if JUCE is rejected)
- Use native audio and plugin layers per OS:
  - CoreAudio/AudioToolbox + AUv3 on Apple;
  - Steinberg VST3 SDK + ASIO/WASAPI on Windows;
  - Oboe on Android.
- Put a single cross-platform UI on top. Flutter can be called from C++ through FFI and adapts
  well to phones and tablets.
- Cost: more platform code to maintain, and plugin editor embedding is harder.

## Explicitly rejected
- **Four fully native UIs** (SwiftUI, AppKit, WinUI, Compose). That would mean four separate
  UI codebases, which contradicts the single-codebase requirement and makes visual drift likely.
