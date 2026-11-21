# Ynot for iPhone & iPad: native shell (planned, milestone M4)

**Status: planned. Nothing in this folder runs yet.** See `docs/ROADMAP.md`.

What this shell will contain (everything else comes from `ynot_core`, cross-compiled as a
static library):
- `AVAudioSessionDevice`: an `ynot::AudioDevice` built on RemoteIO. Handles interruptions,
  route changes and background audio.
- `AUv3PluginHost`: hosts AUv3 extensions only. Desktop VST3 and AU **cannot** load on iOS,
  and the compatibility report explains this to the user.
- Touch-first UI:
  - iPhone: Compact layout with bottom tabs.
  - iPad: Expanded layout with a sidebar, Apple Pencil editing, and pointer and keyboard
    support.
- StoreKit 2 and Sign in with Apple.
