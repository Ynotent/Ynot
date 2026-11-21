# Ynot for iPhone & iPad: platform notes (planned, milestone M4)

**Status: planned. Nothing in this folder runs yet.** See `docs/ROADMAP.md`.

Shared app code is in `/app` (JUCE). The platform-specific pieces are:
- `AVAudioSessionDevice`: an `ynot::AudioDevice` built on RemoteIO. Handles interruptions,
  route changes and background audio.
- `AUv3PluginHost`: hosts AUv3 extensions only. Desktop VST3 and AU **cannot** load on iOS,
  and the compatibility report explains this to the user.
- Touch-first UI:
  - iPhone: Compact layout with bottom tabs.
  - iPad: Expanded layout with a sidebar, Apple Pencil editing, and pointer and keyboard
    support.
- StoreKit 2 and Sign in with Apple.

> In code, the audio and plugin pieces above are `app/src/JuceAudioDevice` and
> `app/src/JucePluginHost`, shared by all platforms and configured for each by JUCE.
