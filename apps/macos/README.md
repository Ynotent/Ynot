# Ynot for macOS: native shell (planned, milestone M1)

**Status: planned. Nothing in this folder runs yet.** See `docs/ROADMAP.md`.

What this shell will contain (everything else comes from `ynot_core`):
- `CoreAudioDevice`: an `ynot::AudioDevice` built on AUHAL. Supports aggregate devices and
  reports driver latency.
- `MacPluginHost`: an `ynot::PluginHost` for AUv2, AUv3 and VST3. It runs plugins in an
  out-of-process helper and embeds their editors as NSViews.
- The app UI, built from `design/tokens.json` and `ynot::decideLayout()` (Studio/Expanded),
  plus the macOS menu bar and key commands.
- A document-based `.ynot` package, iCloud-independent Ynot Cloud sync, and StoreKit (Mac App
  Store build) or web checkout (direct build).
