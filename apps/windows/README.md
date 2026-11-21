# Ynot for Windows: native shell (planned, milestone M2)

**Status: planned. Nothing in this folder runs yet.** See `docs/ROADMAP.md`.

What this shell will contain (everything else comes from `ynot_core`):
- `AsioDevice` and `WasapiDevice` (exclusive, then shared): implementations of
  `ynot::AudioDevice` that handle hot-plug and sample-rate changes.
- `VstPluginHost`: an `ynot::PluginHost` for VST3, running plugins out of process. That process
  is required on ARM64 to host x64 plugins.
- The app UI, built from `design/tokens.json` and `ynot::decideLayout()`, with title-bar menus
  and pen and touch support for touchscreen laptops.
- Credential Locker for tokens and web checkout for subscriptions.
