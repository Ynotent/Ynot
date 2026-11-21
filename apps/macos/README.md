# Ynot for macOS: platform notes (planned, milestone M1)

**Status: planned. Nothing in this folder runs yet.** See `docs/ROADMAP.md`.

Shared app code is in `/app` (JUCE). The platform-specific pieces are:
- `CoreAudioDevice`: an `ynot::AudioDevice` built on AUHAL. Supports aggregate devices and
  reports driver latency.
- `MacPluginHost`: an `ynot::PluginHost` for AUv2, AUv3 and VST3. It runs plugins in an
  out-of-process helper and embeds their editors as NSViews.
- The app UI, built from `design/tokens.json` and `ynot::decideLayout()` (Studio/Expanded),
  plus the macOS menu bar and key commands.
- A document-based `.ynot` package, iCloud-independent Ynot Cloud sync, and StoreKit (Mac App
  Store build) or web checkout (direct build).

> In code, the audio and plugin pieces above are `app/src/JuceAudioDevice` and
> `app/src/JucePluginHost`, shared by all platforms and configured for each by JUCE.
